#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "utils.h"
#include "logger.h"
#include <QFontDatabase>
#include <QGraphicsOpacityEffect>
#include "sys/mount.h"
#include <QDesktopWidget>
#include <QApplication>
#include <QWSServer>
#include <QFile>
#include "targetlist.h"
#include "target.h"
#include <QTranslator>
#include <QThread>
#include "extractworker.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)

{
    ui->setupUi(this);
    this->setFixedSize(this->size());
    /* Set up logging */
    logger = new Logger();
    logger->addLine("Starting OSMC installer");
    /* UI set up */
    #ifdef Q_WS_QWS
    QWSServer *server = QWSServer::instance();
    if(server)
        server->setCursorVisible(false);
        server->setBackground(QBrush(Qt::black));
        this->setWindowFlags(Qt::Tool|Qt::CustomizeWindowHint);
    #endif
    this->setGeometry(QStyle::alignedRect(Qt::LeftToRight, Qt::AlignCenter, this->size(), qApp->desktop()->availableGeometry()));
    QFontDatabase fontDatabase;
    fontDatabase.addApplicationFont(":/assets/resources/SourceSansPro-Regular.ttf");
    QGraphicsOpacityEffect *ope = new QGraphicsOpacityEffect(this);
    ope->setOpacity(0.5);
    ui->statusLabel->setGraphicsEffect(ope);
    ui->copyrightLabel->setGraphicsEffect(ope);
    ui->statusProgressBar->setGraphicsEffect(ope);
    /* Populate target list map */
    targetList = new TargetList();
}


void MainWindow::install()
{
    /* Find out what device we are running on */
    logger->addLine("Detecting device we are running on");
    device = targetList->getTarget(utils::getOSMCDev());
    if (device == NULL)
    {
        haltInstall("unsupported device"); /* No tr here as not got lang yet */
        return;
    }
    /* Mount the BOOT filesystem */
    logger->addLine("Mounting boot filesystem");
    if (! utils::mountPartition(device, MNT_BOOT))
    {
        haltInstall("could not mount bootfs");
        return;
    }
    /* Sanity check: need filesystem.tar.xz */
    QFile fileSystem("/mnt/boot/filesystem.tar.xz");
    if (! fileSystem.exists())
    {
        haltInstall("no filesystem found");
        return;
    }
    /* Load in preseeded values */
    preseed = new PreseedParser();
    if (preseed->isLoaded())
    {
        logger->addLine("Preseed file found, will attempt to parse");
        /* Locales */
        locale = preseed->getStringValue("globe/locale");
        if (! locale.isEmpty())
        {
            logger->addLine("Found a definition for globalisation: " + locale);
            QTranslator translator;
            if (translator.load(qApp->applicationDirPath() + "/osmc_" + locale + ".qm"))
            {
                logger->addLine("Translation loaded successfully!");
                qApp->installTranslator(&translator);
                ui->retranslateUi(this);
            }
            else
                logger->addLine("Could not load translation");
        }
        /* Install target */
        installTarget = preseed->getStringValue("target/storage");
        if (! installTarget.isEmpty())
        {
            logger->addLine("Found a definition for storage: " + installTarget);
            if (installTarget == "nfs")
            {
                QString nfsPath = preseed->getStringValue("target/storagePath");
                if (! nfsPath.isEmpty())
                {
                    device->setRoot(nfsPath);
                    useNFS = true;
                }
            }
            if (installTarget == "usb")
            {
                /* Behaviour for handling USB installs */
                if (utils::getOSMCDev() == "rbp") { device->setRoot("/dev/sda1"); }
            }
        }
        /* Bring up network if using NFS */
        if (useNFS)
        {
            logger->addLine("NFS installation chosen, must bring up network");
            nw = new Network();
            nw->setIP(preseed->getStringValue("network/ip"));
            nw->setMask(preseed->getStringValue("network/mask"));
            nw->setGW(preseed->getStringValue("network/gw"));
            nw->setDNS1(preseed->getStringValue("network/dns1"));
            nw->setDNS2(preseed->getStringValue("network/dns2"));
            if (! nw->isDefined())
            {
                logger->addLine("Either network preseed definition incomplete, or user wants DHCP");
                nw->setAuto();
                logger->addLine("Attempting to bring up eth0");
                ui->statusLabel->setText(tr("Configuring Network"));
                nw->bringUp();
            }
        }
    }

    else
    {
        logger->addLine("No preseed file was found");
    }
    /* If !nfs, create necessary partitions */
    ui->statusLabel->setText(tr("Partitioning device"));
    if (! useNFS)
    {
        logger->addLine("Creating root partition");
        if (device->hasRootChanged())
        {
            logger->addLine("Must mklabel as root fs is on another device");
            utils::mklabel(device->getRoot().remove(QRegExp("\d")), false);
            utils::mkpart(device->getRoot(), "ext4", "4096s", "100%");
            utils::fmtpart(device->getRoot(), "ext4");
        }
        else
        {
            utils::mkpart(device->getRoot(), "ext4", "258M", "100%");
            utils::fmtpart(device->getRoot(), "ext4");
        }
    }
    /* Mount root filesystem */
    if (useNFS)
        bc = new BootloaderConfig(device, nw);
    else
        bc = new BootloaderConfig(device, NULL);
    logger->addLine("Mounting root");
    if ( ! utils::mountPartition(device, MNT_ROOT))
    {
        logger->addLine("Error occured trying to mount root of " + device->getRoot());
        haltInstall(tr("can't mount root"));
    }
   /* Extract root filesystem */
   ui->statusLabel->setText(tr("Installing files"));
   logger->addLine("Extracting files to root filesystem");
   ui->statusProgressBar->setMinimum(0);
   ui->statusProgressBar->setMaximum(100);
   QThread* thread = new QThread;
   ExtractWorker *worker = new ExtractWorker(fileSystem.fileName(), MNT_ROOT);
   worker->moveToThread(thread);
   connect(thread, SIGNAL(started()), worker, SLOT(extract()));
   connect(worker, SIGNAL(progressUpdate(unsigned)), this, SLOT(setProgress(unsigned)));
   connect(worker, SIGNAL(error(QString)), this, SLOT(haltInstall(QString)));
   connect(worker, SIGNAL(finished()), thread, SLOT(quit()));
   connect(worker, SIGNAL(finished()), worker, SLOT(deleteLater()));
   connect(thread, SIGNAL(finished()), thread, SLOT(deleteLater()));
   connect(thread, SIGNAL(finished()), this, SLOT(finished()));
}

void MainWindow::setupBootLoader()
{
    /* Set up the boot loader */
       ui->statusLabel->setText(tr("Configuring bootloader"));
       logger->addLine("Configuring bootloader: moving /boot to appropriate boot partition");
       bc->copyBootFiles();
       logger->addLine("Configuring boot cmdline");
       bc->configureCmdline();
       logger->addLine("Configuring /etc/fstab");
       bc->configureFstab();
       /* Dump the log */
       logger->addLine("Successful installation. Dumping log and rebooting system");
       dumpLog();
       /* Reboot */
       utils::rebootSystem();
}

void MainWindow::haltInstall(QString errorMsg)
{
    logger->addLine("Halting Install. Error message was: " + errorMsg);
    ui->statusProgressBar->setMaximum(100);
    ui->statusProgressBar->setValue(0);
    ui->statusLabel->setText(tr("Install failed: ") + errorMsg);
    dumpLog();
}

void MainWindow::dumpLog()
{
    QFile logFile("/mnt/boot/install.log");
    utils::writeToFile(logFile, logger->getLog(), false);
}

void MainWindow::finished()
{
    logger->addLine("Extraction of root filesystem completed");
    logger->addLine("Configuring bootloader");
    setupBootLoader();
}

void MainWindow::setProgress(unsigned value)
{
    ui->statusProgressBar->setValue(value);
}

MainWindow::~MainWindow()
{
    delete ui;
}
