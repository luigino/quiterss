﻿#include <QtCore>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#endif

#include "aboutdialog.h"
#include "addfeedwizard.h"
#include "db_func.h"
#include "delegatewithoutfocus.h"
#include "feedpropertiesdialog.h"
#include "filterrulesdialog.h"
#include "newsfiltersdialog.h"
#include "optionsdialog.h"
#include "rsslisting.h"
#include "webpage.h"
#include "VersionNo.h"

/*! \brief Обработка сообщений полученных из запущщеной копии программы *******/
void RSSListing::receiveMessage(const QString& message)
{
  qDebug() << QString("Received message: '%1'").arg(message);
  if (!message.isEmpty()){
    QStringList params = message.split('\n');
    foreach (QString param, params) {
      if (param == "--show") slotShowWindows();
      if (param == "--exit") slotClose();
      if (param.contains("feed:", Qt::CaseInsensitive)) {
        QClipboard *clipboard = QApplication::clipboard();
        if (param.contains("https://", Qt::CaseInsensitive)) {
          param.remove(0, 5);
          clipboard->setText(param);
        } else {
          param.remove(0, 7);
          clipboard->setText("http://" + param);
        }
        activateWindow();
        addFeed();
      }
    }
  }
}

/*!****************************************************************************/
RSSListing::RSSListing(QSettings *settings, QString dataDirPath, QWidget *parent)
  : QMainWindow(parent),
    settings_(settings),
    dataDirPath_(dataDirPath)
{
  setWindowTitle(QString("QuiteRSS v%1").arg(STRPRODUCTVER));
  setContextMenuPolicy(Qt::CustomContextMenu);

  dbFileName_ = dataDirPath_ + QDir::separator() + kDbName;
  QString versionDB = initDB(dbFileName_);
  settings_->setValue("VersionDB", versionDB);

  storeDBMemory_ = settings_->value("Settings/storeDBMemory", true).toBool();
  storeDBMemoryT_ = storeDBMemory_;

  db_ = QSqlDatabase::addDatabase("QSQLITE");
  if (storeDBMemory_)
    db_.setDatabaseName(":memory:");
  else
    db_.setDatabaseName(dbFileName_);
  db_.open();

  if (storeDBMemory_) {
    dbMemFileThread_ = new DBMemFileThread(this);
    dbMemFileThread_->sqliteDBMemFile(db_, dbFileName_, false);
    dbMemFileThread_->start(QThread::NormalPriority);
    while(dbMemFileThread_->isRunning()) qApp->processEvents();
  }

  persistentUpdateThread_ = new UpdateThread(this);
  persistentUpdateThread_->setObjectName("persistentUpdateThread_");
  connect(this, SIGNAL(startGetUrlTimer()),
          persistentUpdateThread_, SIGNAL(startGetUrlTimer()));
  connect(persistentUpdateThread_, SIGNAL(readedXml(QByteArray, QUrl)),
          this, SLOT(receiveXml(QByteArray, QUrl)));
  connect(persistentUpdateThread_, SIGNAL(getUrlDone(int,QDateTime)),
          this, SLOT(getUrlDone(int,QDateTime)));

  persistentParseThread_ = new ParseThread(this, &db_);
  persistentParseThread_->setObjectName("persistentParseThread_");
  connect(this, SIGNAL(xmlReadyParse(QByteArray,QUrl)),
          persistentParseThread_, SLOT(parseXml(QByteArray,QUrl)),
          Qt::QueuedConnection);

  cleanUp();

  currentNewsTab = NULL;
  newsView_ = NULL;
  webView_ = NULL;
  notificationWidget = NULL;
  feedIdOld = -2;
  openingLink_ = false;

  createFeedsDock();
  createToolBarNull();

  createActions();
  createShortcut();
  createMenu();
  createToolBar();
  createMenuFeed();

  createStatusBar();
  createTray();

  tabWidget_ = new QTabWidget(this);
  tabWidget_->setObjectName("tabWidget_");
  tabWidget_->setFocusPolicy(Qt::NoFocus);
  tabWidget_->setMovable(true);

  connect(tabWidget_, SIGNAL(tabCloseRequested(int)),
          this, SLOT(slotTabCloseRequested(int)));
  connect(tabWidget_, SIGNAL(currentChanged(int)),
          this, SLOT(slotTabCurrentChanged(int)));
  connect(this, SIGNAL(signalCurrentTab(int,bool)),
          SLOT(setCurrentTab(int,bool)), Qt::QueuedConnection);

  tabBar_ = qFindChild<QTabBar*>(tabWidget_);
  tabBar_->installEventFilter(this);

  tabCurrentUpdateOff_ = false;

  setCentralWidget(tabWidget_);

  connect(this, SIGNAL(signalCloseApp()),
          SLOT(slotCloseApp()), Qt::QueuedConnection);
  commitDataRequest_ = false;
  connect(qApp, SIGNAL(commitDataRequest(QSessionManager&)),
          this, SLOT(slotCommitDataRequest(QSessionManager&)));

  faviconLoader = new FaviconLoader(this);
  connect(this, SIGNAL(startGetUrlTimer()),
          faviconLoader, SIGNAL(startGetUrlTimer()));
  connect(faviconLoader, SIGNAL(signalIconRecived(const QString&, const QByteArray &)),
          this, SLOT(slotIconFeedLoad(const QString&, const QByteArray &)));

  connect(this, SIGNAL(signalShowNotification()),
          SLOT(showNotification()), Qt::QueuedConnection);

  loadSettingsFeeds();

  setStyleSheet("QMainWindow::separator { width: 1px; }");

  readSettings();

  if (autoUpdatefeedsStartUp_) slotGetAllFeeds();
  int updateFeedsTime = autoUpdatefeedsTime_*60000;
  if (autoUpdatefeedsInterval_ == 1)
    updateFeedsTime = updateFeedsTime*60;
  updateFeedsTimer_.start(updateFeedsTime, this);

  QTimer::singleShot(10000, this, SLOT(slotUpdateAppChacking()));

  translator_ = new QTranslator(this);
  appInstallTranslator();

  installEventFilter(this);
}

/*!****************************************************************************/
RSSListing::~RSSListing()
{
  qDebug("App_Closing");

  persistentUpdateThread_->quit();
  persistentParseThread_->quit();
  faviconLoader->quit();

  QSqlQuery q(db_);

  db_.transaction();
  bool cleanUpDB = false;
  q.exec("SELECT value FROM info WHERE name='cleanUpAllDB_0.10.0'");
  if (q.next()) cleanUpDB = q.value(0).toBool();
  else q.exec("INSERT INTO info(name, value) VALUES ('cleanUpAllDB_0.10.0', 'true')");

  q.exec("SELECT id FROM feeds");
  while (q.next()) {
    QString feedId = q.value(0).toString();
    QSqlQuery qt(db_);
    QString qStr = QString("UPDATE news SET new=0 WHERE feedId=='%1' AND new=1")
        .arg(feedId);
    qt.exec(qStr);
    qStr = QString("UPDATE news SET read=2 WHERE feedId=='%1' AND read=1").
        arg(feedId);
    qt.exec(qStr);

    feedsCleanUp(feedId);

    qStr = QString("UPDATE news SET description='', content='', received='', "
                   "author_name='', author_uri='', author_email='', "
                   "category='', new='', read='', starred='', deleted=2 "
                   "WHERE feedId=='%1'").
        arg(feedId);
    if (cleanUpDB) qStr.append(" AND deleted=1");
    else qStr.append(" AND deleted!=0");
    qt.exec(qStr);
  }

  q.exec("UPDATE feeds SET newCount=0");
  q.exec("VACUUM");
  q.finish();
  db_.commit();

  if (storeDBMemory_) {
    dbMemFileThread_->sqliteDBMemFile(db_, dbFileName_, true);
    dbMemFileThread_->start();
    while(dbMemFileThread_->isRunning());
  }

  while (persistentUpdateThread_->isRunning());
  while (persistentParseThread_->isRunning());
  while (faviconLoader->isRunning());

  db_.close();

  QSqlDatabase::removeDatabase(QString());
}

void RSSListing::slotCommitDataRequest(QSessionManager &manager)
{
  manager.release();
  commitDataRequest_ = true;
}

/*virtual*/ void RSSListing::showEvent(QShowEvent*)
{
  connect(feedsDock_, SIGNAL(dockLocationChanged(Qt::DockWidgetArea)),
          this, SLOT(slotDockLocationChanged(Qt::DockWidgetArea)), Qt::UniqueConnection);
}

/*!****************************************************************************/
bool RSSListing::eventFilter(QObject *obj, QEvent *event)
{
  static int deactivateState = 0;

  static bool tabFixed = false;
  if (obj == feedsView_->viewport()) {
    if (event->type() == QEvent::ToolTip) {
      return true;
    }
    return false;
  } else if (obj == toolBarNull_) {
    if (event->type() == QEvent::MouseButtonRelease) {
      slotVisibledFeedsDock();
    }
    return false;
  } else if (obj == tabBar_) {
    if (event->type() == QEvent::MouseButtonPress) {
      QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
      if (mouseEvent->button() & Qt::MiddleButton) {
        slotTabCloseRequested(tabBar_->tabAt(mouseEvent->pos()));
      } else if (mouseEvent->button() & Qt::LeftButton) {
        if (tabBar_->tabAt(QPoint(mouseEvent->pos().x(), 0)) == 0)
          tabFixed = true;
        else
          tabFixed = false;
      }
    } else if (event->type() == QEvent::MouseMove) {
      QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
      if (mouseEvent->buttons() & Qt::LeftButton) {
        if ((tabBar_->tabAt(QPoint(mouseEvent->pos().x()-78, 0)) <= 0) || tabFixed)
          return true;
      }
    }
    return false;
  } else if (obj == statusBar()) {
    if (event->type() == QEvent::MouseButtonRelease) {
      if (windowState() & Qt::WindowMaximized) {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
        if ((mouseEvent->pos().x() > (statusBar()->width()-statusBar()->height())) &&
            (mouseEvent->pos().y() > 0)) {
          setFullScreen();
        }
      }
    }
    return false;
  }
  // Обработка открытия ссылки во внешнем браузере в фоне
  else if ((event->type() == QEvent::WindowDeactivate) && (openingLink_)) {
    openingLink_ = false;
    timerLinkOpening_.start(openingLinkTimeout_, this);
    deactivateState = 1;
    return QMainWindow::eventFilter(obj, event);
  }
  // Отрисовалась деактивация
  else if ((deactivateState == 1) && (event->type() == QEvent::Paint)) {
    deactivateState = 2;
    return QMainWindow::eventFilter(obj, event);
  }
  // Деактивация произведена. Переактивируемся
  else if ((deactivateState == 2) && timerLinkOpening_.isActive()) {
    deactivateState = 3;
    if (!isActiveWindow()) {
      setWindowState(windowState() & ~Qt::WindowActive);
      show();
      raise();
      activateWindow();
    }
    return QMainWindow::eventFilter(obj, event);
  }
  // Отрисовалась активация
  else if ((deactivateState == 3) && (event->type() == QEvent::Paint)) {
    deactivateState = 0;
    return QMainWindow::eventFilter(obj, event);
  } else {
    // pass the event on to the parent class
    return QMainWindow::eventFilter(obj, event);
  }
}

/*! \brief ОБработка событий закрытия окна ************************************/
/*virtual*/ void RSSListing::closeEvent(QCloseEvent* event)
{
  event->ignore();
  if (closingTray_ && !commitDataRequest_ && showTrayIcon_) {
    oldState = windowState();
    emit signalPlaceToTray();
  } else {
    slotClose();
  }
}

/*! \brief Обработка события выхода из приложения *****************************/
void RSSListing::slotClose()
{
  traySystem->hide();
  hide();
  writeSettings();
  saveActionShortcuts();
  emit signalCloseApp();
}

/*! \brief Завершение приложения **********************************************/
void RSSListing::slotCloseApp()
{
  qApp->quit();
}

/*! \brief Обработка события изменения состояния окна *************************/
/*virtual*/ void RSSListing::changeEvent(QEvent *event)
{
  if(event->type() == QEvent::WindowStateChange) {
    if(isMinimized()) {
      oldState = ((QWindowStateChangeEvent*)event)->oldState();
      if (minimizingTray_ && showTrayIcon_) {
        event->ignore();
        emit signalPlaceToTray();
        return;
      }
    } else {
      oldState = windowState();

      qDebug() << QString::number(oldState, 16)
          << QString::number(((QWindowStateChangeEvent*)event)->oldState(), 16);

      if (tabWidget_->count() &&
          !(((QWindowStateChangeEvent*)event)->oldState() & Qt::WindowMinimized)) {
        QString stateStr;
        if (((QWindowStateChangeEvent*)event)->oldState() & Qt::WindowMaximized)
          stateStr = "Maximized";
        QStringList widthStrList;
        for (int i = 0; i < currentNewsTab->newsHeader_->count(); i++) {
          widthStrList << QString::number(currentNewsTab->newsHeader_->sectionSize(i));
        }
        settings_->setValue("NewsHeaderSectionSize" + stateStr, widthStrList);

        stateStr = "";
        if (windowState() & Qt::WindowMaximized)
          stateStr = "Maximized";
        int logicalIndex = 0;
        widthStrList = settings_->value("NewsHeaderSectionSize" + stateStr).toStringList();
        foreach (const QString &widthStr, widthStrList) {
          currentNewsTab->newsHeader_->resizeSection(logicalIndex++, widthStr.toInt());
        }
      }
    }
  } else if(event->type() == QEvent::ActivationChange) {
    if (isActiveWindow() && (behaviorIconTray_ == CHANGE_ICON_TRAY)) {
#if defined(QT_NO_DEBUG_OUTPUT)
      traySystem->setIcon(QIcon(":/images/quiterss16"));
#else
      traySystem->setIcon(QIcon(":/images/quiterssDebug"));
#endif
    }
  } else if(event->type() == QEvent::LanguageChange) {
    retranslateStrings();
  }
  QMainWindow::changeEvent(event);
}

/*! \brief Обработка события помещения программы в трей ***********************/
void RSSListing::slotPlaceToTray()
{
  hide();
  if (emptyWorking_)
    QTimer::singleShot(10000, this, SLOT(myEmptyWorkingSet()));
  if (markReadMinimize_)
    setFeedRead(currentNewsTab->feedId_, 2);
  if (clearStatusNew_)
    markAllFeedsOld();
  idFeedList_.clear();
  cntNewNewsList_.clear();

  if (storeDBMemory_) {
    dbMemFileThread_->sqliteDBMemFile(db_, dbFileName_, true);
    dbMemFileThread_->start(QThread::LowestPriority);
  }
  writeSettings();
}

/*! \brief Обработка событий трея *********************************************/
void RSSListing::slotActivationTray(QSystemTrayIcon::ActivationReason reason)
{
  switch (reason) {
  case QSystemTrayIcon::Unknown:
    break;
  case QSystemTrayIcon::Context:
    trayMenu_->activateWindow();
    break;
  case QSystemTrayIcon::DoubleClick:
    if (!singleClickTray_) slotShowWindows(true);
    break;
  case QSystemTrayIcon::Trigger:
    if (singleClickTray_) slotShowWindows(true);
    break;
  case QSystemTrayIcon::MiddleClick:
    break;
  }
}

/*! \brief Отображение окна по событию ****************************************/
void RSSListing::slotShowWindows(bool trayClick)
{
  if (!trayClick || isHidden()){
    if (oldState & Qt::WindowMaximized) {
      showMaximized();
    } else {
      showNormal();
    }
    activateWindow();
  } else {
    emit signalPlaceToTray();
  }
}

void RSSListing::timerEvent(QTimerEvent *event)
{
  if (event->timerId() == updateFeedsTimer_.timerId()) {
    updateFeedsTimer_.stop();
    int updateFeedsTime = autoUpdatefeedsTime_*60000;
    if (autoUpdatefeedsInterval_ == 1)
      updateFeedsTime = updateFeedsTime*60;
    updateFeedsTimer_.start(updateFeedsTime, this);
    slotTimerUpdateFeeds();
  }
  // Отбработка передачи ссылки во внешний браузер фоном
  else if (event->timerId() == timerLinkOpening_.timerId()) {
    timerLinkOpening_.stop();
    if (!isActiveWindow()) {
      setWindowState(windowState() & ~Qt::WindowActive);
      show();
      raise();
      activateWindow();
    }
    setFocus();
    qDebug() << "----------------------------------------------";
  }
}

void RSSListing::createFeedsDock()
{
  feedsModel_ = new FeedsModel(this);
  feedsModel_->setTable("feeds");
  feedsModel_->select();
  feedsTreeModel_ = new FeedsTreeModel("feeds",
      QStringList() << QObject::tr("ID") << QObject::tr("PARENTID") << QObject::tr("TEXT")
          << QObject::tr("UNREAD") << QObject::tr("UNDELETECOUNT") << QObject::tr("UPDATED"),
      QStringList() << "id" << "parentId" << "text" << "unread" << "undeleteCount" << "updated",
      0,
      "text");

  feedsView_ = new FeedsView(this);
  feedsView_->setFrameStyle(QFrame::NoFrame);
  feedsView_->setModel(feedsModel_);
  for (int i = 0; i < feedsModel_->columnCount(); ++i)
    feedsView_->hideColumn(i);
  feedsView_->showColumn(feedsModel_->fieldIndex("text"));
  feedsView_->header()->setResizeMode(feedsModel_->fieldIndex("text"), QHeaderView::Stretch);
  feedsView_->header()->setResizeMode(feedsModel_->fieldIndex("unread"), QHeaderView::ResizeToContents);
  feedsView_->header()->setResizeMode(feedsModel_->fieldIndex("undeleteCount"), QHeaderView::ResizeToContents);
  feedsView_->header()->setResizeMode(feedsModel_->fieldIndex("updated"), QHeaderView::ResizeToContents);
  feedsTreeView_ = new FeedsTreeView(this);
  feedsTreeView_->setFrameStyle(QFrame::NoFrame);
  feedsTreeView_->setModel(feedsTreeModel_);
  for (int i = 0; i < feedsTreeModel_->columnCount(); ++i)
    feedsTreeView_->hideColumn(i);
  feedsTreeView_->showColumn(feedsTreeModel_->proxyColumnByOriginal("text"));
  feedsTreeView_->header()->setResizeMode(feedsTreeModel_->proxyColumnByOriginal("text"), QHeaderView::Stretch);
  feedsTreeView_->header()->setResizeMode(feedsTreeModel_->proxyColumnByOriginal("unread"), QHeaderView::ResizeToContents);
  feedsTreeView_->header()->setResizeMode(feedsTreeModel_->proxyColumnByOriginal("undeleteCount"), QHeaderView::ResizeToContents);
  feedsTreeView_->header()->setResizeMode(feedsTreeModel_->proxyColumnByOriginal("updated"), QHeaderView::ResizeToContents);

  feedsTreeView_->sortByColumn(feedsTreeView_->columnIndex("id"),Qt::AscendingOrder);
  feedsTreeView_->setColumnHidden("id", true);
  feedsTreeView_->setColumnHidden("parentId", true);
  feedsTreeView_->setSelectionBehavior(QAbstractItemView::SelectRows);
  feedsTreeView_->setSelectionMode(QAbstractItemView::SingleSelection);
  feedsTreeView_->setDragDropMode(QAbstractItemView::InternalMove);
  feedsTreeView_->setDragEnabled(true);
  feedsTreeView_->setAcceptDrops(true);
  feedsTreeView_->setDropIndicatorShown(true);

  //! Create title DockWidget
  feedsTitleLabel_ = new QLabel(this);
  feedsTitleLabel_->setObjectName("feedsTitleLabel_");
  feedsTitleLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
  feedsTitleLabel_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);

  feedsToolBar_ = new QToolBar(this);
  feedsToolBar_->setStyleSheet("QToolBar { border: none; padding: 1px; }");
  feedsToolBar_->setIconSize(QSize(18, 18));

  QHBoxLayout *feedsPanelLayout = new QHBoxLayout();
  feedsPanelLayout->setMargin(0);
  feedsPanelLayout->setSpacing(0);
  feedsPanelLayout->addWidget(feedsTitleLabel_, 0);
  feedsPanelLayout->addStretch(1);
  feedsPanelLayout->addWidget(feedsToolBar_, 0);
  feedsPanelLayout->addSpacing(5);

  QWidget *feedsPanel = new QWidget(this);
  feedsPanel->setObjectName("feedsPanel");
  feedsPanel->setLayout(feedsPanelLayout);

  //! Create feeds DockWidget
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setDockOptions(QMainWindow::AnimatedDocks|QMainWindow::AllowNestedDocks);

  findFeeds_ = new FindFeed(this);
  QVBoxLayout *findFeedsLayout = new QVBoxLayout();
  findFeedsLayout->setMargin(2);
  findFeedsLayout->addWidget(findFeeds_);
  findFeedsWidget_ = new QWidget(this);
  findFeedsWidget_->hide();
  findFeedsWidget_->setLayout(findFeedsLayout);

  QVBoxLayout *feedsLayout = new QVBoxLayout();
  feedsLayout->setMargin(0);
  feedsLayout->setSpacing(0);
  feedsLayout->addWidget(findFeedsWidget_);
  feedsLayout->addWidget(feedsView_, 1);
  feedsLayout->addSpacing(4);
  feedsLayout->addWidget(feedsTreeView_, 1);
  QFrame *feedsWidget_ = new QFrame(this);
  feedsWidget_->setFrameStyle(QFrame::Panel | QFrame::Sunken);
  feedsWidget_->setLayout(feedsLayout);

  feedsDock_ = new QDockWidget(this);
  feedsDock_->setObjectName("feedsDock");
  feedsDock_->setAllowedAreas(Qt::LeftDockWidgetArea|Qt::RightDockWidgetArea|Qt::TopDockWidgetArea);
  feedsDock_->setFeatures(QDockWidget::DockWidgetMovable);
  feedsDock_->setTitleBarWidget(feedsPanel);
  feedsDock_->setWidget(feedsWidget_);
  addDockWidget(Qt::LeftDockWidgetArea, feedsDock_);

  connect(feedsView_, SIGNAL(pressed(QModelIndex)),
          this, SLOT(slotFeedsTreeClicked(QModelIndex)));
  connect(feedsView_, SIGNAL(signalMiddleClicked()),
          this, SLOT(slotOpenFeedNewTab()));
  connect(feedsView_, SIGNAL(pressKeyUp()), this, SLOT(slotFeedUpPressed()));
  connect(feedsView_, SIGNAL(pressKeyDown()), this, SLOT(slotFeedDownPressed()));
  connect(feedsView_, SIGNAL(pressKeyHome()), this, SLOT(slotFeedHomePressed()));
  connect(feedsView_, SIGNAL(pressKeyEnd()), this, SLOT(slotFeedEndPressed()));
  connect(feedsView_, SIGNAL(customContextMenuRequested(QPoint)),
          this, SLOT(showContextMenuFeed(const QPoint &)));
  connect(feedsTreeView_, SIGNAL(pressed(QModelIndex)),
          this, SLOT(slotFeedClicked(QModelIndex)));
  connect(feedsDock_, SIGNAL(dockLocationChanged(Qt::DockWidgetArea)),
          this, SLOT(slotFeedsDockLocationChanged(Qt::DockWidgetArea)));

  connect(findFeeds_, SIGNAL(textChanged(QString)),
          this, SLOT(slotFindFeeds(QString)));
  connect(findFeeds_, SIGNAL(signalSelectFind()),
          this, SLOT(slotSelectFind()));
  connect(findFeeds_, SIGNAL(returnPressed()),
          this, SLOT(slotSelectFind()));

  feedsView_->viewport()->installEventFilter(this);
}

void RSSListing::createToolBarNull()
{
  toolBarNull_ = new QToolBar(this);
  toolBarNull_->setObjectName("toolBarNull");
  toolBarNull_->setMovable(false);
  toolBarNull_->setFixedWidth(6);
  addToolBar(Qt::LeftToolBarArea, toolBarNull_);

  pushButtonNull_ = new QPushButton(this);
  pushButtonNull_->setObjectName("pushButtonNull");
  pushButtonNull_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  pushButtonNull_->setFocusPolicy(Qt::NoFocus);
  toolBarNull_->addWidget(pushButtonNull_);
  connect(pushButtonNull_, SIGNAL(clicked()), this, SLOT(slotVisibledFeedsDock()));
  toolBarNull_->installEventFilter(this);

  connect(feedsDock_, SIGNAL(visibilityChanged(bool)),
          this, SLOT(updateIconToolBarNull(bool)));
}

void RSSListing::createNewsTab(int index)
{
  currentNewsTab = (NewsTabWidget*)tabWidget_->widget(index);
  currentNewsTab->setSettings();
  currentNewsTab->retranslateStrings();
  currentNewsTab->setBrowserPosition();

  newsModel_ = currentNewsTab->newsModel_;
  newsView_ = currentNewsTab->newsView_;  
  webView_ = currentNewsTab->webView_;
}

void RSSListing::createStatusBar()
{
  progressBar_ = new QProgressBar(this);
  progressBar_->setObjectName("progressBar_");
  progressBar_->setFixedWidth(100);
  progressBar_->setFixedHeight(14);
  progressBar_->setMinimum(0);
  progressBar_->setMaximum(0);
  progressBar_->setTextVisible(false);
  progressBar_->setVisible(false);
  statusBar()->setMinimumHeight(22);

  QToolButton *fullScreenButton = new QToolButton(this);
  fullScreenButton->setDefaultAction(fullScreenAct_);
  fullScreenButton->setStyleSheet("QToolButton { border: none; padding: 0px; }");
  statusBar()->installEventFilter(this);

  statusBar()->addPermanentWidget(progressBar_);
  statusUnread_ = new QLabel(this);
  statusBar()->addPermanentWidget(statusUnread_);
  statusAll_ = new QLabel(this);
  statusBar()->addPermanentWidget(statusAll_);
  statusBar()->addPermanentWidget(fullScreenButton);
  statusBar()->setVisible(true);
}

void RSSListing::createTray()
{
#if defined(QT_NO_DEBUG_OUTPUT)
    traySystem = new QSystemTrayIcon(QIcon(":/images/quiterss16"), this);
#else
  traySystem = new QSystemTrayIcon(QIcon(":/images/quiterssDebug"), this);
#endif
  connect(traySystem,SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
          this, SLOT(slotActivationTray(QSystemTrayIcon::ActivationReason)));
  connect(this, SIGNAL(signalPlaceToTray()),
          this, SLOT(slotPlaceToTray()), Qt::QueuedConnection);
  traySystem->setToolTip("QuiteRSS");
  createTrayMenu();
}

/*! \brief Создание действий **************************************************
 * \details Которые будут использоваться в главном меню и ToolBar
 ******************************************************************************/
void RSSListing::createActions()
{
  addFeedAct_ = new QAction(this);
  addFeedAct_->setObjectName("addFeedAct");
  addFeedAct_->setIcon(QIcon(":/images/add"));
  connect(addFeedAct_, SIGNAL(triggered()), this, SLOT(addFeed()));

  addFolderAct_ = new QAction(this);
  addFolderAct_->setObjectName("addCategoryAct");
  addFolderAct_->setIcon(QIcon(":/images/addCategory"));
  connect(addFolderAct_, SIGNAL(triggered()), this, SLOT(addFolder()));

  openFeedNewTabAct_ = new QAction(this);
  openFeedNewTabAct_->setObjectName("openNewTabAct");
  connect(openFeedNewTabAct_, SIGNAL(triggered()), this, SLOT(slotOpenFeedNewTab()));

  deleteFeedAct_ = new QAction(this);
  deleteFeedAct_->setObjectName("deleteFeedAct");
  deleteFeedAct_->setIcon(QIcon(":/images/delete"));
  connect(deleteFeedAct_, SIGNAL(triggered()), this, SLOT(deleteFeed()));

  importFeedsAct_ = new QAction(this);
  importFeedsAct_->setObjectName("importFeedsAct");
  importFeedsAct_->setIcon(QIcon(":/images/importFeeds"));
  connect(importFeedsAct_, SIGNAL(triggered()), this, SLOT(slotImportFeeds()));

  exportFeedsAct_ = new QAction(this);
  exportFeedsAct_->setObjectName("exportFeedsAct");
  exportFeedsAct_->setIcon(QIcon(":/images/exportFeeds"));
  connect(exportFeedsAct_, SIGNAL(triggered()), this, SLOT(slotExportFeeds()));

  exitAct_ = new QAction(this);
  exitAct_->setObjectName("exitAct");
  connect(exitAct_, SIGNAL(triggered()), this, SLOT(slotClose()));

  mainToolbarToggle_ = new QAction(this);
  mainToolbarToggle_->setCheckable(true);
  newsToolbarToggle_ = new QAction(this);
  newsToolbarToggle_->setCheckable(true);
  browserToolbarToggle_ = new QAction(this);
  browserToolbarToggle_->setCheckable(true);

  toolBarStyleI_ = new QAction(this);
  toolBarStyleI_->setObjectName("toolBarStyleI_");
  toolBarStyleI_->setCheckable(true);
  toolBarStyleT_ = new QAction(this);
  toolBarStyleT_->setObjectName("toolBarStyleT_");
  toolBarStyleT_->setCheckable(true);
  toolBarStyleTbI_ = new QAction(this);
  toolBarStyleTbI_->setObjectName("toolBarStyleTbI_");
  toolBarStyleTbI_->setCheckable(true);
  toolBarStyleTuI_ = new QAction(this);
  toolBarStyleTuI_->setObjectName("toolBarStyleTuI_");
  toolBarStyleTuI_->setCheckable(true);
  toolBarStyleTuI_->setChecked(true);

  toolBarIconBig_ = new QAction(this);
  toolBarIconBig_->setObjectName("toolBarIconBig_");
  toolBarIconBig_->setCheckable(true);
  toolBarIconNormal_ = new QAction(this);
  toolBarIconNormal_->setObjectName("toolBarIconNormal_");
  toolBarIconNormal_->setCheckable(true);
  toolBarIconNormal_->setChecked(true);
  toolBarIconSmall_ = new QAction(this);
  toolBarIconSmall_->setObjectName("toolBarIconSmall_");
  toolBarIconSmall_->setCheckable(true);

  toolBarToggle_ = new QAction(this);

  systemStyle_ = new QAction(this);
  systemStyle_->setObjectName("systemStyle_");
  systemStyle_->setCheckable(true);
  system2Style_ = new QAction(this);
  system2Style_->setObjectName("system2Style_");
  system2Style_->setCheckable(true);
  greenStyle_ = new QAction(this);
  greenStyle_->setObjectName("greenStyle_");
  greenStyle_->setCheckable(true);
  greenStyle_->setChecked(true);
  orangeStyle_ = new QAction(this);
  orangeStyle_->setObjectName("orangeStyle_");
  orangeStyle_->setCheckable(true);
  purpleStyle_ = new QAction(this);
  purpleStyle_->setObjectName("purpleStyle_");
  purpleStyle_->setCheckable(true);
  pinkStyle_ = new QAction(this);
  pinkStyle_->setObjectName("pinkStyle_");
  pinkStyle_->setCheckable(true);
  grayStyle_ = new QAction(this);
  grayStyle_->setObjectName("grayStyle_");
  grayStyle_->setCheckable(true);

  topBrowserPositionAct_ = new QAction(this);
  topBrowserPositionAct_->setCheckable(true);
  topBrowserPositionAct_->setData(TOP_POSITION);
  bottomBrowserPositionAct_ = new QAction(this);
  bottomBrowserPositionAct_->setCheckable(true);
  bottomBrowserPositionAct_->setData(BOTTOM_POSITION);
  rightBrowserPositionAct_ = new QAction(this);
  rightBrowserPositionAct_->setCheckable(true);
  rightBrowserPositionAct_->setData(RIGHT_POSITION);
  leftBrowserPositionAct_ = new QAction(this);
  leftBrowserPositionAct_->setCheckable(true);
  leftBrowserPositionAct_->setData(LEFT_POSITION);

  autoLoadImagesToggle_ = new QAction(this);
  autoLoadImagesToggle_->setObjectName("autoLoadImagesToggle");

  printAct_ = new QAction(this);
  printAct_->setObjectName("printAct");
  printAct_->setIcon(QIcon(":/images/printer"));
  this->addAction(printAct_);
  connect(printAct_, SIGNAL(triggered()), this, SLOT(slotPrint()));
  printPreviewAct_ = new QAction(this);
  printPreviewAct_->setObjectName("printPreviewAct");
  printPreviewAct_->setIcon(QIcon(":/images/printer"));
  this->addAction(printPreviewAct_);
  connect(printPreviewAct_, SIGNAL(triggered()), this, SLOT(slotPrintPreview()));

  zoomInAct_ = new QAction(this);
  zoomInAct_->setObjectName("zoomInAct");
  zoomInAct_->setIcon(QIcon(":/images/zoomIn"));
  zoomOutAct_ = new QAction(this);
  zoomOutAct_->setObjectName("zoomOutAct");
  zoomOutAct_->setIcon(QIcon(":/images/zoomOut"));
  zoomTo100Act_ = new QAction(this);
  zoomTo100Act_->setObjectName("zoomTo100Act");

  updateFeedAct_ = new QAction(this);
  updateFeedAct_->setObjectName("updateFeedAct");
  updateFeedAct_->setIcon(QIcon(":/images/updateFeed"));
  connect(updateFeedAct_, SIGNAL(triggered()), this, SLOT(slotGetFeed()));
  connect(feedsView_, SIGNAL(signalDoubleClicked(QModelIndex)),
          updateFeedAct_, SLOT(trigger()));

  updateAllFeedsAct_ = new QAction(this);
  updateAllFeedsAct_->setObjectName("updateAllFeedsAct");
  updateAllFeedsAct_->setIcon(QIcon(":/images/updateAllFeeds"));
  connect(updateAllFeedsAct_, SIGNAL(triggered()), this, SLOT(slotGetAllFeeds()));

  markAllFeedsRead_ = new QAction(this);
  markAllFeedsRead_->setObjectName("markAllFeedRead");
  markAllFeedsRead_->setIcon(QIcon(":/images/markReadAll"));
  connect(markAllFeedsRead_, SIGNAL(triggered()), this, SLOT(markAllFeedsRead()));

  titleSortFeedsAct_ = new QAction(this);
  titleSortFeedsAct_->setCheckable(true);
  connect(titleSortFeedsAct_, SIGNAL(triggered()), this, SLOT(slotSortFeeds()));

  markNewsRead_ = new QAction(this);
  markNewsRead_->setObjectName("markNewsRead");
  markNewsRead_->setIcon(QIcon(":/images/markRead"));

  markAllNewsRead_ = new QAction(this);
  markAllNewsRead_->setObjectName("markAllNewsRead");
  markAllNewsRead_->setIcon(QIcon(":/images/markReadAll"));

  setNewsFiltersAct_ = new QAction(this);
  setNewsFiltersAct_->setIcon(QIcon(":/images/filterOff"));
  connect(setNewsFiltersAct_, SIGNAL(triggered()), this, SLOT(showNewsFiltersDlg()));
  setFilterNewsAct_ = new QAction(this);
  setFilterNewsAct_->setIcon(QIcon(":/images/filterOff"));
  connect(setFilterNewsAct_, SIGNAL(triggered()), this, SLOT(showFilterRulesDlg()));

  optionsAct_ = new QAction(this);
  optionsAct_->setObjectName("optionsAct");
  optionsAct_->setIcon(QIcon(":/images/options"));
  connect(optionsAct_, SIGNAL(triggered()), this, SLOT(showOptionDlg()));

  feedsFilter_ = new QAction(this);
  feedsFilter_->setIcon(QIcon(":/images/filterOff"));
  filterFeedsAll_ = new QAction(this);
  filterFeedsAll_->setObjectName("filterFeedsAll_");
  filterFeedsAll_->setCheckable(true);
  filterFeedsAll_->setChecked(true);
  filterFeedsNew_ = new QAction(this);
  filterFeedsNew_->setObjectName("filterFeedsNew_");
  filterFeedsNew_->setCheckable(true);
  filterFeedsUnread_ = new QAction(this);
  filterFeedsUnread_->setObjectName("filterFeedsUnread_");
  filterFeedsUnread_->setCheckable(true);
  filterFeedsStarred_ = new QAction(this);
  filterFeedsStarred_->setObjectName("filterFeedsStarred_");
  filterFeedsStarred_->setCheckable(true);

  newsFilter_ = new QAction(this);
  newsFilter_->setIcon(QIcon(":/images/filterOff"));
  filterNewsAll_ = new QAction(this);
  filterNewsAll_->setObjectName("filterNewsAll_");
  filterNewsAll_->setCheckable(true);
  filterNewsAll_->setChecked(true);
  filterNewsNew_ = new QAction(this);
  filterNewsNew_->setObjectName("filterNewsNew_");
  filterNewsNew_->setCheckable(true);
  filterNewsUnread_ = new QAction(this);
  filterNewsUnread_->setObjectName("filterNewsUnread_");
  filterNewsUnread_->setCheckable(true);
  filterNewsStar_ = new QAction(this);
  filterNewsStar_->setObjectName("filterNewsStar_");
  filterNewsStar_->setCheckable(true);
  filterNewsNotStarred_ = new QAction(this);
  filterNewsNotStarred_->setObjectName("filterNewsNotStarred_");
  filterNewsNotStarred_->setCheckable(true);
  filterNewsUnreadStar_ = new QAction(this);
  filterNewsUnreadStar_->setObjectName("filterNewsUnreadStar_");
  filterNewsUnreadStar_->setCheckable(true);
  filterNewsDeleted_ = new QAction(this);
  filterNewsDeleted_->setObjectName("filterNewsDeleted_");
  filterNewsDeleted_->setCheckable(true);

  aboutAct_ = new QAction(this);
  aboutAct_->setObjectName("AboutAct_");
  connect(aboutAct_, SIGNAL(triggered()), this, SLOT(slotShowAboutDlg()));

  updateAppAct_ = new QAction(this);
  updateAppAct_->setObjectName("UpdateApp_");
  connect(updateAppAct_, SIGNAL(triggered()), this, SLOT(slotShowUpdateAppDlg()));

  reportProblemAct_ = new QAction(this);
  reportProblemAct_->setObjectName("reportProblemAct_");
  connect(reportProblemAct_, SIGNAL(triggered()), this, SLOT(slotReportProblem()));

  openInBrowserAct_ = new QAction(this);
  openInBrowserAct_->setObjectName("openInBrowserAct");
  this->addAction(openInBrowserAct_);

  openInExternalBrowserAct_ = new QAction(this);
  openInExternalBrowserAct_->setObjectName("openInExternalBrowserAct");
  this->addAction(openInExternalBrowserAct_);

  openNewsNewTabAct_ = new QAction(this);
  openNewsNewTabAct_->setObjectName("openInNewTabAct");
  this->addAction(openNewsNewTabAct_);
  openNewsBackgroundTabAct_ = new QAction(this);
  openNewsBackgroundTabAct_->setObjectName("openInBackgroundTabAct");
  this->addAction(openNewsBackgroundTabAct_);

  markStarAct_ = new QAction(this);
  markStarAct_->setObjectName("markStarAct");
  markStarAct_->setIcon(QIcon(":/images/starOn"));

  deleteNewsAct_ = new QAction(this);
  deleteNewsAct_->setObjectName("deleteNewsAct");
  deleteNewsAct_->setIcon(QIcon(":/images/delete"));
  this->addAction(deleteNewsAct_);
  deleteAllNewsAct_ = new QAction(this);
  deleteAllNewsAct_->setObjectName("deleteAllNewsAct");
//  deleteAllNewsAct_->setIcon(QIcon(":/images/delete"));
  this->addAction(deleteAllNewsAct_);

  restoreNewsAct_ = new QAction(this);
  restoreNewsAct_->setIcon(QIcon(":/images/images/arrow_turn_left.png"));

  markFeedRead_ = new QAction(this);
  markFeedRead_->setObjectName("markFeedRead");
  markFeedRead_->setIcon(QIcon(":/images/markRead"));
  connect(markFeedRead_, SIGNAL(triggered()), this, SLOT(markFeedRead()));

  feedProperties_ = new QAction(this);
  feedProperties_->setObjectName("feedProperties");
  feedProperties_->setIcon(QIcon(":/images/preferencesFeed"));
  connect(feedProperties_, SIGNAL(triggered()), this, SLOT(slotShowFeedPropertiesDlg()));

  feedKeyUpAct_ = new QAction(this);
  feedKeyUpAct_->setObjectName("feedKeyUp");
  connect(feedKeyUpAct_, SIGNAL(triggered()), this, SLOT(slotFeedUpPressed()));
  this->addAction(feedKeyUpAct_);

  feedKeyDownAct_ = new QAction(this);
  feedKeyDownAct_->setObjectName("feedKeyDownAct");
  connect(feedKeyDownAct_, SIGNAL(triggered()), this, SLOT(slotFeedDownPressed()));
  this->addAction(feedKeyDownAct_);

  newsKeyUpAct_ = new QAction(this);
  newsKeyUpAct_->setObjectName("newsKeyUpAct");
  this->addAction(newsKeyUpAct_);

  newsKeyDownAct_ = new QAction(this);
  newsKeyDownAct_->setObjectName("newsKeyDownAct");
  this->addAction(newsKeyDownAct_);

  switchFocusAct_ = new QAction(this);
  switchFocusAct_->setObjectName("switchFocusAct");
  connect(switchFocusAct_, SIGNAL(triggered()), this, SLOT(slotSwitchFocus()));
  this->addAction(switchFocusAct_);

  visibleFeedsDockAct_ = new QAction(this);
  visibleFeedsDockAct_->setObjectName("visibleFeedsDockAct");
  connect(visibleFeedsDockAct_, SIGNAL(triggered()), this, SLOT(slotVisibledFeedsDock()));
  this->addAction(visibleFeedsDockAct_);

  showUnreadCount_ = new QAction(this);
  showUnreadCount_->setData(feedsModel_->fieldIndex("unread"));
  showUnreadCount_->setCheckable(true);
  showUndeleteCount_ = new QAction(this);
  showUndeleteCount_->setData(feedsModel_->fieldIndex("undeleteCount"));
  showUndeleteCount_->setCheckable(true);
  showLastUpdated_ = new QAction(this);
  showLastUpdated_->setData(feedsModel_->fieldIndex("updated"));
  showLastUpdated_->setCheckable(true);

  QAction *openNewsWebViewAct_ = new QAction(this);
  openNewsWebViewAct_->setShortcut(QKeySequence(Qt::Key_Return));
  connect(openNewsWebViewAct_, SIGNAL(triggered()),
          this, SLOT(slotOpenNewsWebView()));
  this->addAction(openNewsWebViewAct_);

  QAction *findTextAct_ = new QAction(this);
  findTextAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_F));
  connect(findTextAct_, SIGNAL(triggered()),
          this, SLOT(findText()));
  this->addAction(findTextAct_);

  placeToTrayAct_ = new QAction(this);
  placeToTrayAct_->setObjectName("placeToTrayAct");
  connect(placeToTrayAct_, SIGNAL(triggered()), this, SLOT(slotPlaceToTray()));
  this->addAction(placeToTrayAct_);

  findFeedAct_ = new QAction(this);
  findFeedAct_->setCheckable(true);
  findFeedAct_->setChecked(false);
  findFeedAct_->setIcon(QIcon(":/images/images/findFeed.png"));
  feedsToolBar_->addAction(findFeedAct_);
  connect(findFeedAct_, SIGNAL(triggered(bool)),
          this, SLOT(findFeedVisible(bool)));

  fullScreenAct_ = new QAction(this);
  fullScreenAct_->setObjectName("fullScreenAct");
  fullScreenAct_->setIcon(QIcon(":/images/images/fullScreen.png"));
  this->addAction(fullScreenAct_);
  connect(fullScreenAct_, SIGNAL(triggered()),
          this, SLOT(setFullScreen()));

  connect(markNewsRead_, SIGNAL(triggered()),
          this, SLOT(markNewsRead()));
  connect(markAllNewsRead_, SIGNAL(triggered()),
          this, SLOT(markAllNewsRead()));
  connect(markStarAct_, SIGNAL(triggered()),
          this, SLOT(markNewsStar()));
  connect(deleteNewsAct_, SIGNAL(triggered()),
          this, SLOT(deleteNews()));
  connect(deleteAllNewsAct_, SIGNAL(triggered()),
          this, SLOT(deleteAllNewsList()));
  connect(restoreNewsAct_, SIGNAL(triggered()),
          this, SLOT(restoreNews()));

  connect(newsKeyUpAct_, SIGNAL(triggered()),
          this, SLOT(slotNewsUpPressed()));
  connect(newsKeyDownAct_, SIGNAL(triggered()),
          this, SLOT(slotNewsDownPressed()));

  connect(openInBrowserAct_, SIGNAL(triggered()),
          this, SLOT(openInBrowserNews()));
  connect(openInExternalBrowserAct_, SIGNAL(triggered()),
          this, SLOT(openInExternalBrowserNews()));
  connect(openNewsNewTabAct_, SIGNAL(triggered()),
          this, SLOT(slotOpenNewsNewTab()));
  connect(openNewsBackgroundTabAct_, SIGNAL(triggered()),
          this, SLOT(slotOpenNewsBackgroundTab()));
}

void RSSListing::createShortcut()
{
  addFeedAct_->setShortcut(QKeySequence(QKeySequence::New));
  listActions_.append(addFeedAct_);
  deleteFeedAct_->setShortcut(QKeySequence());
  listActions_.append(deleteFeedAct_);
  exitAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Q));  // standart on other OS
  listActions_.append(exitAct_);
  updateFeedAct_->setShortcut(QKeySequence(Qt::Key_F5));
  listActions_.append(updateFeedAct_);
  updateAllFeedsAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_F5));
  listActions_.append(updateAllFeedsAct_);
  optionsAct_->setShortcut(QKeySequence(Qt::Key_F8));
  listActions_.append(optionsAct_);
  deleteNewsAct_->setShortcut(QKeySequence(Qt::Key_Delete));
  listActions_.append(deleteNewsAct_);
  deleteAllNewsAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Delete));
  listActions_.append(deleteAllNewsAct_);
  feedProperties_->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_E));
  listActions_.append(feedProperties_);
  feedKeyUpAct_->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_Up));
  listActions_.append(feedKeyUpAct_);
  feedKeyDownAct_->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_Down));
  listActions_.append(feedKeyDownAct_);
  newsKeyUpAct_->setShortcut(QKeySequence(Qt::Key_Left));
  listActions_.append(newsKeyUpAct_);
  newsKeyDownAct_->setShortcut(QKeySequence(Qt::Key_Right));
  listActions_.append(newsKeyDownAct_);

  listActions_.append(importFeedsAct_);
  listActions_.append(exportFeedsAct_);
  listActions_.append(autoLoadImagesToggle_);
  listActions_.append(markAllFeedsRead_);
  listActions_.append(markFeedRead_);
  listActions_.append(markNewsRead_);
  listActions_.append(markAllNewsRead_);
  listActions_.append(markStarAct_);

  listActions_.append(openInBrowserAct_);
  openInBrowserAct_->setShortcut(QKeySequence(Qt::Key_O));
  listActions_.append(openInExternalBrowserAct_);
  openInExternalBrowserAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_O));
  openNewsNewTabAct_->setShortcut(QKeySequence(Qt::Key_T));
  listActions_.append(openNewsNewTabAct_);
  openNewsBackgroundTabAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_T));
  listActions_.append(openNewsBackgroundTabAct_);

  switchFocusAct_->setShortcut(QKeySequence(Qt::CTRL+Qt::Key_Tab));
  listActions_.append(switchFocusAct_);

  visibleFeedsDockAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_D));
  listActions_.append(visibleFeedsDockAct_);

  listActions_.append(placeToTrayAct_);

  zoomInAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Plus));
  listActions_.append(zoomInAct_);
  zoomOutAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_Minus));
  listActions_.append(zoomOutAct_);
  zoomTo100Act_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_0));
  listActions_.append(zoomTo100Act_);

  printAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::Key_P));
  listActions_.append(printAct_);
  printPreviewAct_->setShortcut(QKeySequence(Qt::CTRL + Qt::SHIFT + Qt::Key_P));
  listActions_.append(printPreviewAct_);

  fullScreenAct_->setShortcut(QKeySequence(Qt::Key_F11));
  listActions_.append(fullScreenAct_);

  loadActionShortcuts();
}

void RSSListing::loadActionShortcuts()
{
  settings_->beginGroup("/Shortcuts");

  QListIterator<QAction *> iter(listActions_);
  while (iter.hasNext()) {
    QAction *pAction = iter.next();
    if (pAction->objectName().isEmpty())
      continue;

    listDefaultShortcut_.append(pAction->shortcut());

    const QString& sKey = '/' + pAction->objectName();
    const QString& sValue = settings_->value('/' + sKey, pAction->shortcut().toString()).toString();
    pAction->setShortcut(QKeySequence(sValue));
  }

  settings_->endGroup();
}

void RSSListing::saveActionShortcuts()
{
  settings_->beginGroup("/Shortcuts/");

  QListIterator<QAction *> iter(listActions_);
  while (iter.hasNext()) {
    QAction *pAction = iter.next();
    if (pAction->objectName().isEmpty())
      continue;

    const QString& sKey = '/' + pAction->objectName();
    const QString& sValue = QString(pAction->shortcut());
    settings_->setValue(sKey, sValue);
  }

  settings_->endGroup();
}

/*! \brief Создание главного меню *********************************************/
void RSSListing::createMenu()
{
  fileMenu_ = new QMenu(this);
  menuBar()->addMenu(fileMenu_);
  fileMenu_->addAction(addFeedAct_);
  fileMenu_->addAction(addFolderAct_);
  fileMenu_->addSeparator();
  fileMenu_->addAction(importFeedsAct_);
  fileMenu_->addAction(exportFeedsAct_);
  fileMenu_->addSeparator();
  fileMenu_->addAction(exitAct_);

  editMenu_ = new QMenu(this);
//  menuBar()->addMenu(editMenu_);
  editMenu_->setVisible(false);

  toolbarsMenu_ = new QMenu(this);
  toolbarsMenu_->addAction(mainToolbarToggle_);
  toolbarsMenu_->addAction(newsToolbarToggle_);
  toolbarsMenu_->addAction(browserToolbarToggle_);

  customizeToolbarMenu_ = new QMenu(this);
  toolBarStyleMenu_ = new QMenu(this);
  toolBarStyleMenu_->addAction(toolBarStyleI_);
  toolBarStyleMenu_->addAction(toolBarStyleT_);
  toolBarStyleMenu_->addAction(toolBarStyleTbI_);
  toolBarStyleMenu_->addAction(toolBarStyleTuI_);
  toolBarStyleGroup_ = new QActionGroup(this);
  toolBarStyleGroup_->addAction(toolBarStyleI_);
  toolBarStyleGroup_->addAction(toolBarStyleT_);
  toolBarStyleGroup_->addAction(toolBarStyleTbI_);
  toolBarStyleGroup_->addAction(toolBarStyleTuI_);
  connect(toolBarStyleGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setToolBarStyle(QAction*)));

  toolBarIconSizeMenu_ = new QMenu(this);
  toolBarIconSizeMenu_->addAction(toolBarIconBig_);
  toolBarIconSizeMenu_->addAction(toolBarIconNormal_);
  toolBarIconSizeMenu_->addAction(toolBarIconSmall_);
  toolBarIconSizeGroup_ = new QActionGroup(this);
  toolBarIconSizeGroup_->addAction(toolBarIconBig_);
  toolBarIconSizeGroup_->addAction(toolBarIconNormal_);
  toolBarIconSizeGroup_->addAction(toolBarIconSmall_);
  connect(toolBarIconSizeGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setToolBarIconSize(QAction*)));

  customizeToolbarMenu_->addMenu(toolBarStyleMenu_);
  customizeToolbarMenu_->addMenu(toolBarIconSizeMenu_);

  styleMenu_ = new QMenu(this);
  styleMenu_->addAction(systemStyle_);
  styleMenu_->addAction(system2Style_);
  styleMenu_->addAction(greenStyle_);
  styleMenu_->addAction(orangeStyle_);
  styleMenu_->addAction(purpleStyle_);
  styleMenu_->addAction(pinkStyle_);
  styleMenu_->addAction(grayStyle_);
  styleGroup_ = new QActionGroup(this);
  styleGroup_->addAction(systemStyle_);
  styleGroup_->addAction(system2Style_);
  styleGroup_->addAction(greenStyle_);
  styleGroup_->addAction(orangeStyle_);
  styleGroup_->addAction(purpleStyle_);
  styleGroup_->addAction(pinkStyle_);
  styleGroup_->addAction(grayStyle_);
  connect(styleGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setStyleApp(QAction*)));

  browserPositionMenu_ = new QMenu(this);
  browserPositionMenu_->addAction(topBrowserPositionAct_);
  browserPositionMenu_->addAction(bottomBrowserPositionAct_);
  browserPositionMenu_->addAction(rightBrowserPositionAct_);
  browserPositionMenu_->addAction(leftBrowserPositionAct_);
  browserPositionGroup_ = new QActionGroup(this);
  browserPositionGroup_->addAction(topBrowserPositionAct_);
  browserPositionGroup_->addAction(bottomBrowserPositionAct_);
  browserPositionGroup_->addAction(rightBrowserPositionAct_);
  browserPositionGroup_->addAction(leftBrowserPositionAct_);
  connect(browserPositionGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setBrowserPosition(QAction*)));

  viewMenu_  = new QMenu(this);
  viewMenu_->addMenu(toolbarsMenu_);
  viewMenu_->addMenu(customizeToolbarMenu_);
  viewMenu_->addSeparator();
  viewMenu_->addMenu(browserPositionMenu_);
  viewMenu_->addMenu(styleMenu_);
  viewMenu_->addSeparator();
  viewMenu_->addAction(fullScreenAct_);
  menuBar()->addMenu(viewMenu_);

  feedMenu_ = new QMenu(this);
  menuBar()->addMenu(feedMenu_);
  feedMenu_->addAction(updateFeedAct_);
  feedMenu_->addAction(updateAllFeedsAct_);
  feedMenu_->addSeparator();
  feedMenu_->addAction(markFeedRead_);
  feedMenu_->addAction(markAllFeedsRead_);
  feedMenu_->addSeparator();

  feedsFilterGroup_ = new QActionGroup(this);
  feedsFilterGroup_->setExclusive(true);
  connect(feedsFilterGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setFeedsFilter(QAction*)));

  feedsFilterMenu_ = new QMenu(this);
  feedsFilterMenu_->addAction(filterFeedsAll_);
  feedsFilterGroup_->addAction(filterFeedsAll_);
  feedsFilterMenu_->addSeparator();
  feedsFilterMenu_->addAction(filterFeedsNew_);
  feedsFilterGroup_->addAction(filterFeedsNew_);
  feedsFilterMenu_->addAction(filterFeedsUnread_);
  feedsFilterGroup_->addAction(filterFeedsUnread_);
  feedsFilterMenu_->addAction(filterFeedsStarred_);
  feedsFilterGroup_->addAction(filterFeedsStarred_);

  feedsFilter_->setMenu(feedsFilterMenu_);
  feedMenu_->addAction(feedsFilter_);
  feedsToolBar_->addAction(feedsFilter_);
  feedsFilterAction = NULL;
  connect(feedsFilter_, SIGNAL(triggered()), this, SLOT(slotFeedsFilter()));

  feedsColumnsMenu_ = new QMenu(this);
  feedsColumnsMenu_->addAction(showUnreadCount_);
  feedsColumnsMenu_->addAction(showUndeleteCount_);
  feedsColumnsMenu_->addAction(showLastUpdated_);
  feedMenu_->addMenu(feedsColumnsMenu_);
  feedsColumnsGroup_ = new QActionGroup(this);
  feedsColumnsGroup_->setExclusive(false);
  feedsColumnsGroup_->addAction(showUnreadCount_);
  feedsColumnsGroup_->addAction(showUndeleteCount_);
  feedsColumnsGroup_->addAction(showLastUpdated_);
  connect(feedsColumnsGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(feedsColumnVisible(QAction*)));

  feedMenu_->addAction(titleSortFeedsAct_);

  feedMenu_->addSeparator();
  feedMenu_->addAction(deleteFeedAct_);
  feedMenu_->addSeparator();
  feedMenu_->addAction(feedProperties_);
  feedMenu_->addSeparator();
//  feedMenu_->addAction(editFeedsTree_);
  connect(feedMenu_, SIGNAL(aboutToShow()), this, SLOT(slotFeedMenuShow()));

  newsMenu_ = new QMenu(this);
  menuBar()->addMenu(newsMenu_);
  newsMenu_->addAction(markNewsRead_);
  newsMenu_->addAction(markAllNewsRead_);
  newsMenu_->addSeparator();
  newsMenu_->addAction(markStarAct_);
  newsMenu_->addSeparator();

  newsFilterGroup_ = new QActionGroup(this);
  newsFilterGroup_->setExclusive(true);
  connect(newsFilterGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(setNewsFilter(QAction*)));

  newsFilterMenu_ = new QMenu(this);
  newsFilterMenu_->addAction(filterNewsAll_);
  newsFilterGroup_->addAction(filterNewsAll_);
  newsFilterMenu_->addSeparator();
  newsFilterMenu_->addAction(filterNewsNew_);
  newsFilterGroup_->addAction(filterNewsNew_);
  newsFilterMenu_->addAction(filterNewsUnread_);
  newsFilterGroup_->addAction(filterNewsUnread_);
  newsFilterMenu_->addAction(filterNewsStar_);
  newsFilterGroup_->addAction(filterNewsStar_);
  newsFilterMenu_->addAction(filterNewsNotStarred_);
  newsFilterGroup_->addAction(filterNewsNotStarred_);
  newsFilterMenu_->addAction(filterNewsUnreadStar_);
  newsFilterGroup_->addAction(filterNewsUnreadStar_);
  newsFilterMenu_->addSeparator();
  newsFilterMenu_->addAction(filterNewsDeleted_);
  newsFilterGroup_->addAction(filterNewsDeleted_);

  newsFilter_->setMenu(newsFilterMenu_);
  newsMenu_->addAction(newsFilter_);
  newsFilterAction = NULL;
  connect(newsFilter_, SIGNAL(triggered()), this, SLOT(slotNewsFilter()));

  newsMenu_->addSeparator();
  newsMenu_->addAction(deleteNewsAct_);
  newsMenu_->addAction(deleteAllNewsAct_);

  browserMenu_ = new QMenu(this);
  menuBar()->addMenu(browserMenu_);

  browserZoomMenu_ = new QMenu(this);
  browserZoomMenu_->setIcon(QIcon(":/images/zoom"));
  browserZoomMenu_->addAction(zoomInAct_);
  browserZoomMenu_->addAction(zoomOutAct_);
  browserZoomMenu_->addSeparator();
  browserZoomMenu_->addAction(zoomTo100Act_);
  browserZoomGroup_ = new QActionGroup(this);
  browserZoomGroup_->addAction(zoomInAct_);
  browserZoomGroup_->addAction(zoomOutAct_);
  browserZoomGroup_->addAction(zoomTo100Act_);
  connect(browserZoomGroup_, SIGNAL(triggered(QAction*)),
          this, SLOT(browserZoom(QAction*)));

  browserMenu_->addAction(autoLoadImagesToggle_);
  browserMenu_->addMenu(browserZoomMenu_);
  browserMenu_->addSeparator();
  browserMenu_->addAction(printAct_);
  browserMenu_->addAction(printPreviewAct_);

  toolsMenu_ = new QMenu(this);
  menuBar()->addMenu(toolsMenu_);
  toolsMenu_->addAction(setNewsFiltersAct_);
  toolsMenu_->addSeparator();
  toolsMenu_->addAction(optionsAct_);

  helpMenu_ = new QMenu(this);
  menuBar()->addMenu(helpMenu_);
  helpMenu_->addAction(updateAppAct_);
  helpMenu_->addSeparator();
  helpMenu_->addAction(reportProblemAct_);
  helpMenu_->addAction(aboutAct_);
}

/*! \brief Создание ToolBar ***************************************************/
void RSSListing::createToolBar()
{
  mainToolbarMenu_ = new QMenu(this);
  mainToolbarMenu_->addActions(customizeToolbarMenu_->actions());
  mainToolbarMenu_->addSeparator();
  mainToolbarMenu_->addAction(toolBarToggle_);

  mainToolbar_ = new QToolBar(this);
  addToolBar(mainToolbar_);
  mainToolbar_->setObjectName("ToolBar_General");
  mainToolbar_->setAllowedAreas(Qt::TopToolBarArea);
  mainToolbar_->setMovable(false);
  mainToolbar_->setContextMenuPolicy(Qt::CustomContextMenu);
  mainToolbar_->addAction(addFeedAct_);
  mainToolbar_->addSeparator();
  mainToolbar_->addAction(updateFeedAct_);
  mainToolbar_->addAction(updateAllFeedsAct_);
  mainToolbar_->addSeparator();
  mainToolbar_->addAction(markFeedRead_);
  mainToolbar_->addSeparator();
  mainToolbar_->addAction(autoLoadImagesToggle_);
  mainToolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);

  connect(mainToolbar_, SIGNAL(visibilityChanged(bool)),
          mainToolbarToggle_, SLOT(setChecked(bool)));
  connect(mainToolbarToggle_, SIGNAL(toggled(bool)),
          mainToolbar_, SLOT(setVisible(bool)));
  connect(toolBarToggle_, SIGNAL(triggered()),
          mainToolbar_, SLOT(hide()));
  connect(autoLoadImagesToggle_, SIGNAL(triggered()),
          this, SLOT(setAutoLoadImages()));
  connect(mainToolbar_, SIGNAL(customContextMenuRequested(QPoint)),
          this, SLOT(showContextMenuToolBar(const QPoint &)));
}

/*! \brief Чтение настроек из ini-файла ***************************************/
void RSSListing::readSettings()
{
  settings_->beginGroup("/Settings");

  showSplashScreen_ = settings_->value("showSplashScreen", true).toBool();
  reopenFeedStartup_ = settings_->value("reopenFeedStartup", true).toBool();

  showTrayIcon_ = settings_->value("showTrayIcon", true).toBool();
  startingTray_ = settings_->value("startingTray", false).toBool();
  minimizingTray_ = settings_->value("minimizingTray", true).toBool();
  closingTray_ = settings_->value("closingTray", false).toBool();
  singleClickTray_ = settings_->value("singleClickTray", false).toBool();
  clearStatusNew_ = settings_->value("clearStatusNew", true).toBool();
  emptyWorking_ = settings_->value("emptyWorking", true).toBool();

  QString strLang("en");
  QString strLocalLang = QLocale::system().name().left(2);
  QDir langDir = qApp->applicationDirPath() + "/lang";
  foreach (QString file, langDir.entryList(QStringList("*.qm"), QDir::Files)) {
    if (strLocalLang.contains(file.section('.', 0, 0).section('_', 1), Qt::CaseInsensitive))
      strLang = strLocalLang;
  }
  langFileName_ = settings_->value("langFileName", strLang).toString();

  QString fontFamily = settings_->value("/feedsFontFamily", qApp->font().family()).toString();
  int fontSize = settings_->value("/feedsFontSize", 8).toInt();
  feedsView_->setFont(QFont(fontFamily, fontSize));
  feedsModel_->font_ = feedsView_->font();

  newsFontFamily_ = settings_->value("/newsFontFamily", qApp->font().family()).toString();
  newsFontSize_ = settings_->value("/newsFontSize", 8).toInt();
  panelNewsFontFamily_ = settings_->value("/panelNewsFontFamily", qApp->font().family()).toString();
  panelNewsFontSize_ = settings_->value("/panelNewsFontSize", 8).toInt();
  webFontFamily_ = settings_->value("/WebFontFamily", qApp->font().family()).toString();
  webFontSize_ = settings_->value("/WebFontSize", 12).toInt();
  notificationFontFamily_ = settings_->value("/notificationFontFamily", qApp->font().family()).toString();
  notificationFontSize_ = settings_->value("/notificationFontSize", 8).toInt();

  autoUpdatefeedsStartUp_ = settings_->value("autoUpdatefeedsStartUp", false).toBool();
  autoUpdatefeeds_ = settings_->value("autoUpdatefeeds", false).toBool();
  autoUpdatefeedsTime_ = settings_->value("autoUpdatefeedsTime", 10).toInt();
  autoUpdatefeedsInterval_ = settings_->value("autoUpdatefeedsInterval", 0).toInt();

  openingFeedAction_ = settings_->value("openingFeedAction", 0).toInt();
  openNewsWebViewOn_ = settings_->value("openNewsWebViewOn", true).toBool();

  markNewsReadOn_ = settings_->value("markNewsReadOn", true).toBool();
  markNewsReadTime_ = settings_->value("markNewsReadTime", 0).toInt();
  markReadSwitchingFeed_ = settings_->value("markReadSwitchingFeed", false).toBool();
  markReadClosingTab_ = settings_->value("markReadClosingTab", false).toBool();
  markReadMinimize_ = settings_->value("markReadMinimize", false).toBool();

  showDescriptionNews_ = settings_->value("showDescriptionNews", true).toBool();

  formatDateTime_ = settings_->value("formatDataTime", "dd.MM.yy hh:mm").toString();
  feedsModel_->formatDateTime_ = formatDateTime_;
  feedsTreeModel_->formatDateTime_ = formatDateTime_;

  maxDayCleanUp_ = settings_->value("maxDayClearUp", 30).toInt();
  maxNewsCleanUp_ = settings_->value("maxNewsClearUp", 200).toInt();
  dayCleanUpOn_ = settings_->value("dayClearUpOn", true).toBool();
  newsCleanUpOn_ = settings_->value("newsClearUpOn", true).toBool();
  readCleanUp_ = settings_->value("readClearUp", false).toBool();
  neverUnreadCleanUp_ = settings_->value("neverUnreadClearUp", true).toBool();
  neverStarCleanUp_ = settings_->value("neverStarClearUp", true).toBool();

  externalBrowserOn_ = settings_->value("externalBrowserOn", 0).toInt();
  if (!externalBrowserOn_) {
    openInExternalBrowserAct_->setVisible(true);
    openNewsNewTabAct_->setVisible(true);
    openNewsBackgroundTabAct_->setVisible(true);
  } else {
    QList <QKeySequence> keySequenceList;
    keySequenceList << openInBrowserAct_->shortcut()
                    << openInExternalBrowserAct_->shortcut();
    openInBrowserAct_->setShortcuts(keySequenceList);
    openInExternalBrowserAct_->setVisible(false);
    openNewsNewTabAct_->setVisible(false);
    openNewsBackgroundTabAct_->setVisible(false);
  }
  externalBrowser_ = settings_->value("externalBrowser", "").toString();
  javaScriptEnable_ = settings_->value("javaScriptEnable", true).toBool();
  pluginsEnable_ = settings_->value("pluginsEnable", true).toBool();

  soundNewNews_ = settings_->value("soundNewNews", true).toBool();
  QString soundNotifyPathStr;
#if defined(Q_OS_WIN) || defined(Q_OS_OS2)
    soundNotifyPathStr = QCoreApplication::applicationDirPath() +
        QString("/sound/notification.wav");
#else
    soundNotifyPathStr = "/usr/share/quiterss/sound/notification.wav";
#endif
  soundNotifyPath_ = settings_->value("soundNotifyPath", soundNotifyPathStr).toString();
  showNotifyOn_ = settings_->value("showNotifyOn", true).toBool();
  countShowNewsNotify_ = settings_->value("countShowNewsNotify", 10).toInt();
  widthTitleNewsNotify_ = settings_->value("widthTitleNewsNotify", 300).toInt();
  timeShowNewsNotify_ = settings_->value("timeShowNewsNotify", 10).toInt();
  onlySelectedFeeds_ = settings_->value("onlySelectedFeeds", false).toBool();

  newsToolbarToggle_->setChecked(settings_->value("newsToolbarShow", true).toBool());
  browserToolbarToggle_->setChecked(settings_->value("browserToolbarShow", true).toBool());

  QString str = settings_->value("toolBarStyle", "toolBarStyleTuI_").toString();
  QList<QAction*> listActions = toolBarStyleGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == str) {
      action->setChecked(true);
      break;
    }
  }
  setToolBarStyle(toolBarStyleGroup_->checkedAction());
  str = settings_->value("toolBarIconSize", "toolBarIconNormal_").toString();
  listActions = toolBarIconSizeGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == str) {
      action->setChecked(true);
      break;
    }
  }
  setToolBarIconSize(toolBarIconSizeGroup_->checkedAction());

  str = settings_->value("styleApplication", "defaultStyle_").toString();
  listActions = styleGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == str) {
      action->setChecked(true);
      break;
    }
  }

  showUnreadCount_->setChecked(settings_->value("showUnreadCount", true).toBool());
  showUndeleteCount_->setChecked(settings_->value("showUndeleteCount", false).toBool());
  showLastUpdated_->setChecked(settings_->value("showLastUpdated", false).toBool());
  feedsColumnVisible(showUnreadCount_);
  feedsColumnVisible(showUndeleteCount_);
  feedsColumnVisible(showLastUpdated_);

  titleSortFeedsAct_->setChecked(settings_->value("sortFeeds", true).toBool());
  slotSortFeeds();

  browserPosition_ = settings_->value("browserPosition", BOTTOM_POSITION).toInt();
  switch (browserPosition_) {
  case TOP_POSITION:   topBrowserPositionAct_->setChecked(true); break;
  case RIGHT_POSITION: rightBrowserPositionAct_->setChecked(true); break;
  case LEFT_POSITION:  leftBrowserPositionAct_->setChecked(true); break;
  default: bottomBrowserPositionAct_->setChecked(true);
  }

  openLinkInBackground_ = settings_->value("openLinkInBackground", true).toBool();
  openingLinkTimeout_ = settings_->value("openingLinkTimeout", 1000).toInt();

  settings_->endGroup();

  resize(800, 600);
  restoreGeometry(settings_->value("GeometryState").toByteArray());
  restoreState(settings_->value("ToolBarsState").toByteArray());

  toolBarNull_->setStyleSheet("QToolBar { border: none; padding: 0px;}");
  if (feedsDockArea_ == Qt::LeftDockWidgetArea) {
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleL.png"));
  } else if (feedsDockArea_ == Qt::RightDockWidgetArea) {
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleR.png"));
  } else {
    pushButtonNull_->setIcon(QIcon(":/images/images/triangleR.png"));
  }

  networkProxy_.setType(static_cast<QNetworkProxy::ProxyType>(
                          settings_->value("networkProxy/type", QNetworkProxy::DefaultProxy).toInt()));
  networkProxy_.setHostName(settings_->value("networkProxy/hostName", "").toString());
  networkProxy_.setPort(    settings_->value("networkProxy/port",     "").toUInt());
  networkProxy_.setUser(    settings_->value("networkProxy/user",     "").toString());
  networkProxy_.setPassword(settings_->value("networkProxy/password", "").toString());
  persistentUpdateThread_->setProxy(networkProxy_);
}

/*! \brief Запись настроек в ini-файл *****************************************/
void RSSListing::writeSettings()
{
  settings_->beginGroup("/Settings");

  settings_->setValue("showSplashScreen", showSplashScreen_);
  settings_->setValue("reopenFeedStartup", reopenFeedStartup_);

  settings_->setValue("storeDBMemory", storeDBMemoryT_);

  settings_->setValue("showTrayIcon", showTrayIcon_);
  settings_->setValue("startingTray", startingTray_);
  settings_->setValue("minimizingTray", minimizingTray_);
  settings_->setValue("closingTray", closingTray_);
  settings_->setValue("behaviorIconTray", behaviorIconTray_);
  settings_->setValue("singleClickTray", singleClickTray_);
  settings_->setValue("clearStatusNew", clearStatusNew_);
  settings_->setValue("emptyWorking", emptyWorking_);

  settings_->setValue("langFileName", langFileName_);

  QString fontFamily = feedsView_->font().family();
  settings_->setValue("/feedsFontFamily", fontFamily);
  int fontSize = feedsView_->font().pointSize();
  settings_->setValue("/feedsFontSize", fontSize);

  settings_->setValue("/newsFontFamily", newsFontFamily_);
  settings_->setValue("/newsFontSize", newsFontSize_);
  settings_->setValue("/panelNewsFontFamily", panelNewsFontFamily_);
  settings_->setValue("/panelNewsFontSize", panelNewsFontSize_);
  settings_->setValue("/WebFontFamily", webFontFamily_);
  settings_->setValue("/WebFontSize", webFontSize_);
  settings_->setValue("/notificationFontFamily", notificationFontFamily_);
  settings_->setValue("/notificationFontSize", notificationFontSize_);

  settings_->setValue("autoUpdatefeedsStartUp", autoUpdatefeedsStartUp_);
  settings_->setValue("autoUpdatefeeds", autoUpdatefeeds_);
  settings_->setValue("autoUpdatefeedsTime", autoUpdatefeedsTime_);
  settings_->setValue("autoUpdatefeedsInterval", autoUpdatefeedsInterval_);

  settings_->setValue("openingFeedAction", openingFeedAction_);
  settings_->setValue("openNewsWebViewOn", openNewsWebViewOn_);

  settings_->setValue("markNewsReadOn", markNewsReadOn_);
  settings_->setValue("markNewsReadTime", markNewsReadTime_);
  settings_->setValue("markReadSwitchingFeed", markReadSwitchingFeed_);
  settings_->setValue("markReadClosingTab", markReadClosingTab_);
  settings_->setValue("markReadMinimize", markReadMinimize_);

  settings_->setValue("showDescriptionNews", showDescriptionNews_);

  settings_->setValue("formatDataTime", formatDateTime_);

  settings_->setValue("maxDayClearUp", maxDayCleanUp_);
  settings_->setValue("maxNewsClearUp", maxNewsCleanUp_);
  settings_->setValue("dayClearUpOn", dayCleanUpOn_);
  settings_->setValue("newsClearUpOn", newsCleanUpOn_);
  settings_->setValue("readClearUp", readCleanUp_);
  settings_->setValue("neverUnreadClearUp", neverUnreadCleanUp_);
  settings_->setValue("neverStarClearUp", neverStarCleanUp_);

  settings_->setValue("externalBrowserOn", externalBrowserOn_);
  settings_->setValue("externalBrowser", externalBrowser_);
  settings_->setValue("javaScriptEnable", javaScriptEnable_);
  settings_->setValue("pluginsEnable", pluginsEnable_);

  settings_->setValue("soundNewNews", soundNewNews_);
  settings_->setValue("soundNotifyPath", soundNotifyPath_);
  settings_->setValue("showNotifyOn", showNotifyOn_);
  settings_->setValue("countShowNewsNotify", countShowNewsNotify_);
  settings_->setValue("widthTitleNewsNotify", widthTitleNewsNotify_);
  settings_->setValue("timeShowNewsNotify", timeShowNewsNotify_);
  settings_->setValue("onlySelectedFeeds", onlySelectedFeeds_);

  settings_->setValue("newsToolbarShow", newsToolbarToggle_->isChecked());
  settings_->setValue("browserToolbarShow", browserToolbarToggle_->isChecked());

  settings_->setValue("toolBarStyle",
                      toolBarStyleGroup_->checkedAction()->objectName());
  settings_->setValue("toolBarIconSize",
                      toolBarIconSizeGroup_->checkedAction()->objectName());

  settings_->setValue("styleApplication",
                      styleGroup_->checkedAction()->objectName());

  settings_->setValue("showUnreadCount", showUnreadCount_->isChecked());
  settings_->setValue("showUndeleteCount", showUndeleteCount_->isChecked());
  settings_->setValue("showLastUpdated", showLastUpdated_->isChecked());

  settings_->setValue("sortFeeds", titleSortFeedsAct_->isChecked());

  settings_->setValue("browserPosition", browserPosition_);

  settings_->setValue("openLinkInBackground", openLinkInBackground_);
  settings_->setValue("openingLinkTimeout", openingLinkTimeout_);

  settings_->endGroup();

  settings_->setValue("GeometryState", saveGeometry());
  settings_->setValue("ToolBarsState", saveState());
  if (tabWidget_->count()) {
    QString stateStr;
    if(isMinimized()) {
      if (oldState & Qt::WindowMaximized)
        stateStr = "Maximized";
    } else {
      if (windowState() & Qt::WindowMaximized)
        stateStr = "Maximized";
    }
    QStringList widthStrList;
    for (int i = 0; i < currentNewsTab->newsHeader_->count(); i++) {
      widthStrList << QString::number(currentNewsTab->newsHeader_->sectionSize(i));
    }
    settings_->setValue("NewsHeaderSectionSize" + stateStr, widthStrList);
    settings_->setValue("NewsHeaderState", currentNewsTab->newsHeader_->saveState());

    settings_->setValue("NewsTabSplitter",
                        currentNewsTab->newsTabWidgetSplitter_->saveGeometry());
    settings_->setValue("NewsTabSplitter",
                        currentNewsTab->newsTabWidgetSplitter_->saveState());
  }

  settings_->setValue("networkProxy/type",     networkProxy_.type());
  settings_->setValue("networkProxy/hostName", networkProxy_.hostName());
  settings_->setValue("networkProxy/port",     networkProxy_.port());
  settings_->setValue("networkProxy/user",     networkProxy_.user());
  settings_->setValue("networkProxy/password", networkProxy_.password());

  NewsTabWidget* widget = (NewsTabWidget*)tabWidget_->widget(0);

  settings_->setValue("feedSettings/currentId", widget->feedId_);
  settings_->setValue("feedSettings/filterName",
                      feedsFilterGroup_->checkedAction()->objectName());
  settings_->setValue("newsSettings/filterName",
                      newsFilterGroup_->checkedAction()->objectName());
}

/*! \brief Добавление ленты в список лент *************************************/
void RSSListing::addFeed()
{
  AddFeedWizard *addFeedWizard = new AddFeedWizard(this, &db_);

  if (addFeedWizard->exec() == QDialog::Rejected) {
    delete addFeedWizard;
    return;
  }

  updateFeedsCount_ = -1;
  idFeedList_.clear();
  cntNewNewsList_.clear();

  emit startGetUrlTimer();
  faviconLoader->requestUrl(addFeedWizard->htmlUrlString_,
                            addFeedWizard->feedUrlString_);
  slotUpdateFeed(addFeedWizard->feedUrlString_, true);

  delete addFeedWizard;
}

void RSSListing::addFolder()
{
  QSqlQuery q(db_);

  // Вычисляем номер ряда для папки, вставляемой в корень
  int rowToParent = 0;
  q.exec("SELECT max(rowToParent) FROM feeds WHERE parentId=0");
  qDebug() << q.lastQuery();
  qDebug() << q.lastError();
  if (q.next() && !q.value(0).isNull()) rowToParent = q.value(0).toInt() + 1;

  // Добавляем папку
  q.prepare("INSERT INTO feeds(text, created, rowToParent) "
            "VALUES (:text, :feedCreateTime, :rowToParent)");
  q.bindValue(":text", "New folder");
  q.bindValue(":feedCreateTime",
              QLocale::c().toString(QDateTime::currentDateTimeUtc(), "yyyy-MM-ddTHH:mm:ss"));
  q.bindValue(":rowToParent", rowToParent);
  q.exec();
  q.finish();

  feedsTreeModel_->refresh();
}

/*! \brief Удаление ленты из списка лент с подтверждением *********************/
void RSSListing::deleteFeed()
{
  if (feedsView_->selectIndex.isValid()) {
    int id = feedsModel_->record(
          feedsView_->selectIndex.row()).field("id").value().toInt();

    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setWindowTitle(tr("Delete Feed"));
    msgBox.setText(QString(tr("Are you sure to delete the feed '%1'?")).
                   arg(feedsModel_->record(feedsView_->selectIndex.row())
                       .field("text").value().toString()));
    msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    msgBox.setDefaultButton(QMessageBox::No);

    if (msgBox.exec() == QMessageBox::No) return;

    db_.transaction();
    QSqlQuery q(db_);
    q.exec(QString("DELETE FROM feeds WHERE id='%1'").arg(id));
    q.exec(QString("DELETE FROM news WHERE feedId='%1'").arg(id));
    q.exec("VACUUM");
    q.finish();
    db_.commit();

    int rowFeeds = feedsView_->currentIndex().row();
    int currentId = feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("id")).data().toInt();
    feedsModel_->select();

    if (currentId == id) {
      if (feedsModel_->rowCount() == rowFeeds) rowFeeds--;
    } else {
      rowFeeds = -1;
      for (int i = 0; i < feedsModel_->rowCount(); i++) {
        if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data().toInt() == currentId) {
          rowFeeds = i;
        }
      }
    }
    feedsView_->updateCurrentIndex(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));
    slotFeedsTreeClicked(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));
  }
  feedsTreeModel_->refresh();
}

/*! \brief Импорт лент из OPML-файла ******************************************/
void RSSListing::slotImportFeeds()
{
  playSoundNewNews_ = false;

  QString fileName = QFileDialog::getOpenFileName(this, tr("Select OPML-File"),
                                                  QDir::homePath(),
                                                  tr("OPML-Files (*.opml *.xml)"));

  if (fileName.isNull()) {
    statusBar()->showMessage(tr("Import canceled"), 3000);
    return;
  }

  qDebug() << "import file:" << fileName;

  QFile file(fileName);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    statusBar()->showMessage(tr("Import: can't open a file"), 3000);
    return;
  }

  db_.transaction();

  QXmlStreamReader xml(&file);

  int elementCount = 0;
  int outlineCount = 0;
  int requestUrlCount = 0;
  while (!xml.atEnd()) {
    xml.readNext();
    if (xml.isStartElement()) {
      statusBar()->showMessage(QVariant(elementCount).toString(), 3000);
      // Выбираем одни outline'ы
      if (xml.name() == "outline") {
        qDebug() << outlineCount << "+:" << xml.prefix().toString()
                 << ":" << xml.name().toString();

        if (!xml.attributes().value("xmlUrl").isEmpty()) {
          QSqlQuery q(db_);

          QString textString(xml.attributes().value("text").toString());
          QString xmlUrlString(xml.attributes().value("xmlUrl").toString());
          bool duplicateFound = false;
          q.exec("select xmlUrl from feeds");
          while (q.next()) {
            if (q.record().value(0).toString() == xmlUrlString) {
              duplicateFound = true;
              break;
            }
          }

          if (duplicateFound) {
            qDebug() << "duplicate feed:" << xmlUrlString << textString;
          } else {
            QString qStr = QString("INSERT INTO feeds(text, title, description, xmlUrl, htmlUrl) "
                                   "VALUES(?, ?, ?, ?, ?)");
            q.prepare(qStr);
            q.addBindValue(textString);
            q.addBindValue(xml.attributes().value("title").toString());
            q.addBindValue(xml.attributes().value("description").toString());
            q.addBindValue(xmlUrlString);
            q.addBindValue(xml.attributes().value("htmlUrl").toString());
            q.exec();
            qDebug() << q.lastQuery() << q.boundValues();
            qDebug() << q.lastError().number() << ": " << q.lastError().text();
            q.finish();

            persistentUpdateThread_->requestUrl(xmlUrlString, QDateTime());
            faviconLoader->requestUrl(
                  xml.attributes().value("htmlUrl").toString(), xmlUrlString);
            requestUrlCount++;
          }
        }
      }
    } else if (xml.isEndElement()) {
      if (xml.name() == "outline") {
        ++outlineCount;
      }
      ++elementCount;
    }
  }
  if (xml.error()) {
    statusBar()->showMessage(QString("Import error: Line=%1, ErrorString=%2").
                             arg(xml.lineNumber()).arg(xml.errorString()), 3000);
  } else {
    statusBar()->showMessage(QString("Import: file read done"), 3000);
  }
  db_.commit();

  file.close();

  if (requestUrlCount) {
    updateAllFeedsAct_->setEnabled(false);
    updateFeedAct_->setEnabled(false);
    showProgressBar(requestUrlCount);
  }

  QModelIndex index = feedsView_->currentIndex();
  feedsModel_->select();
  feedsView_->updateCurrentIndex(index);
}
/*! Экспорт ленты в OPML-файл *************************************************/
void RSSListing::slotExportFeeds()
{
  QString fileName = QFileDialog::getSaveFileName(this, tr("Select OPML-File"),
                                                  QDir::homePath(),
                                                  tr("OPML-Files (*.opml)"));

  if (fileName.isNull()) {
    statusBar()->showMessage(tr("Export canceled"), 3000);
    return;
  }

  QFile file(fileName);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    statusBar()->showMessage(tr("Export: can't open a file"), 3000);
    return;
  }

  QXmlStreamWriter xml(&file);
  xml.setAutoFormatting(true);
  xml.writeStartDocument();
  xml.writeStartElement("opml");
  xml.writeAttribute("version", "2.0");
  xml.writeStartElement("head");
  xml.writeTextElement("title", "QuiteRSS");
  xml.writeTextElement("dateModified", QDateTime::currentDateTime().toString());
  xml.writeEndElement(); // </head>

  QSqlQuery q(db_);
  q.exec("select * from feeds where xmlUrl is not null");

  xml.writeStartElement("body");
  while (q.next()) {
    QString value = q.record().value(q.record().indexOf("text")).toString();
    xml.writeEmptyElement("outline");
    xml.writeAttribute("text", value);
    value = q.record().value(q.record().indexOf("htmlUrl")).toString();
    xml.writeAttribute("htmlUrl", value);
    value = q.record().value(q.record().indexOf("xmlUrl")).toString();
    xml.writeAttribute("xmlUrl", value);
  }
  xml.writeEndElement(); // </body>

  xml.writeEndElement(); // </opml>
  xml.writeEndDocument();

  file.close();
}
/*! \brief приём xml-файла ****************************************************/
void RSSListing::receiveXml(const QByteArray &data, const QUrl &url)
{
  url_ = url;
  data_.append(data);
}

/*! \brief Обработка окончания запроса ****************************************/
void RSSListing::getUrlDone(const int &result, const QDateTime &dtReply)
{
  qDebug() << "getUrl result =" << result;

  if (!url_.isEmpty()) {
    if (!data_.isEmpty()) {
      emit xmlReadyParse(data_, url_);
      QSqlQuery q = db_.exec(QString("update feeds set lastBuildDate = '%1' where xmlUrl == '%2'").
                             arg(dtReply.toString(Qt::ISODate)).
                             arg(url_.toString()));
      qDebug() << url_.toString() << dtReply.toString(Qt::ISODate);
      qDebug() << q.lastQuery() << q.lastError() << q.lastError().text();
    } else {
      slotUpdateFeed(url_, false);
    }
  }
  data_.clear();
  url_.clear();

  // очередь запросов пуста
  if (0 == result) {
//    if (showMessageOn_) { // result=0 может приходить несколько раз
//      statusBar()->showMessage(QString(tr("Update done")), 3000);
//      showMessageOn_ = false;
//    }
//    updateAllFeedsAct_->setEnabled(true);
//    updateFeedAct_->setEnabled(true);
//    progressBar_->hide();
//    progressBar_->setValue(0);
//    progressBar_->setMaximum(0);
  }
  // в очереди запросов осталось _result_ запросов
  else if (0 < result) {
//    progressBar_->setValue(progressBar_->maximum() - result);
//    if (showMessageOn_)
//      statusBar()->showMessage(progressBar_->text());
  }
}

/*! Обновление счётчиков ленты:
 *    количество непрочитанных новостей,
 *    количество новых новостей
 ******************************************************************************/
void RSSListing::recountFeedCounts(int feedId, QModelIndex index)
{
  QSqlQuery q(db_);
  QString qStr;

  db_.transaction();
  //! Подсчет всех новостей (не помеченных удаленными)
  int undeleteCount = 0;
  qStr = QString("SELECT count(id) FROM news WHERE feedId=='%1' AND deleted==0").
      arg(feedId);
  q.exec(qStr);
  if (q.next()) undeleteCount = q.value(0).toInt();

  //! Подсчет непрочитанных новостей
  int unreadCount = 0;
  qStr = QString("SELECT count(read) FROM news WHERE feedId=='%1' AND read==0 AND deleted==0").
      arg(feedId);
  q.exec(qStr);
  if (q.next()) unreadCount = q.value(0).toInt();

  //! Подсчет новых новостей
  int newCount = 0;
  qStr = QString("SELECT count(new) FROM news WHERE feedId='%1' AND new==1 AND deleted==0").
      arg(feedId);
  q.exec(qStr);
  if (q.next()) newCount = q.value(0).toInt();

  //! Установка количества непрочитанных новостей в ленту
  //! Установка количества новых новостей в ленту

  qDebug() << __FUNCTION__ << __LINE__ << index;

  qStr = QString("UPDATE feeds SET unread='%1', newCount='%2', undeleteCount='%3' "
      "WHERE id=='%4'").
      arg(unreadCount).arg(newCount).arg(undeleteCount).arg(feedId);
  q.exec(qStr);
  db_.commit();
}

void RSSListing::slotUpdateFeed(const QUrl &url, const bool &changed)
{
  if (updateFeedsCount_ > 0) {
    updateFeedsCount_--;
    progressBar_->setValue(progressBar_->maximum() - updateFeedsCount_);
    if (showMessageOn_)
      statusBar()->showMessage(progressBar_->text());
  }
  if (updateFeedsCount_ == 0) {
    emit signalShowNotification();
    if (showMessageOn_) {
      statusBar()->showMessage(QString(tr("Update done")), 3000);
      showMessageOn_ = false;
    }
    updateAllFeedsAct_->setEnabled(true);
    updateFeedAct_->setEnabled(true);
    progressBar_->hide();
    progressBar_->setValue(0);
    progressBar_->setMaximum(0);
  }

  if (!changed) return;

  //! Ппоиск идентификатора ленты в таблице лент по URL
  //! + достаем предыдущее значение количества новых новостей
  int parseFeedId = 0;
  int newCountOld = 0;
  QSqlQuery q(db_);
  q.exec(QString("SELECT id, newCount FROM feeds WHERE xmlUrl LIKE '%1'").
         arg(url.toString()));
  if (q.next()) {
    parseFeedId = q.value(q.record().indexOf("id")).toInt();
    newCountOld = q.value(q.record().indexOf("newCount")).toInt();
  }

  //! Устанавливаем время обновления ленты
  q.prepare("UPDATE feeds SET updated=? WHERE id=?");
  q.addBindValue(QLocale::c().toString(QDateTime::currentDateTimeUtc(),
                                       "yyyy-MM-ddTHH:mm:ss"));
  q.addBindValue(parseFeedId);
  q.exec();

  setUserFilter(parseFeedId);

  recountFeedCounts(parseFeedId);

  //! Достаём новое значение количества новых новостей
  int newCount = 0;
  q.exec(QString("SELECT newCount FROM feeds WHERE id=='%1'").arg(parseFeedId));
  if (q.next()) newCount = q.value(0).toInt();

  //! Действия после получения новых новостей: трей, звук
  if (!isActiveWindow() && (newCount > newCountOld) &&
      (behaviorIconTray_ == CHANGE_ICON_TRAY)) {
    traySystem->setIcon(QIcon(":/images/quiterss16_NewNews"));
  }
  refreshInfoTray();
  if (newCount > newCountOld) {
    playSoundNewNews();
  }

  if (isActiveWindow()) {
    idFeedList_.clear();
    cntNewNewsList_.clear();
  }
  if (((newCount - newCountOld) > 0) && !isActiveWindow()) {
    if (onlySelectedFeeds_) {
      q.exec(QString("SELECT value FROM feeds_ex WHERE feedId='%1' AND name='showNotification'").
             arg(parseFeedId));
      if (q.next()) {
        if (q.value(0).toInt() == 1) {
          idFeedList_.append(parseFeedId);
          cntNewNewsList_.append(newCount - newCountOld);
        }
      }
    } else {
      idFeedList_.append(parseFeedId);
      cntNewNewsList_.append(newCount - newCountOld);
    }
  }

  feedsView_->selectIndex = feedsView_->currentIndex();

  // если обновлена просматриваемая лента, кликаем по ней, чтобы обновить просмотр
  if (parseFeedId == currentNewsTab->feedId_) {
    slotUpdateNews();
    slotUpdateStatus();
  }
  // иначе обновляем модель лент
  else {
    feedsModelReload();
  }
}

//! Обновление списка новостей
void RSSListing::slotUpdateNews()
{
  int newsId = newsModel_->index(
        newsView_->currentIndex().row(), newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();

  newsModel_->select();

  if (newsModel_->rowCount() != 0) {
    while (newsModel_->canFetchMore())
      newsModel_->fetchMore();
  }

  int newsRow = -1;
  for (int i = 0; i < newsModel_->rowCount(); i++) {
    if (newsModel_->index(i, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() == newsId) {
      newsRow = i;
    }
  }
  newsView_->setCurrentIndex(newsModel_->index(newsRow, newsModel_->fieldIndex("title")));
  if (newsRow == -1) {
    currentNewsTab->hideWebContent();
  }
}

/*! \brief Обработка нажатия в дереве лент ************************************/
void RSSListing::slotFeedsTreeClicked(QModelIndex index)
{
  int indexTab = -1;

  for (int i = 0; i < tabWidget_->count(); i++) {
    NewsTabWidget *widget = (NewsTabWidget*)tabWidget_->widget(i);
    if (widget->feedId_ == feedsModel_->index(index.row(), feedsModel_->fieldIndex("id")).data().toInt()) {
      indexTab = i;
      break;
    }
  }

  if ((feedsModel_->index(index.row(), feedsModel_->fieldIndex("id")).data() != feedIdOld) ||
      (indexTab == -1)) {
    if (tabWidget_->currentIndex() != 0) {
      tabWidget_->setCurrentIndex(0);
      feedsView_->setCurrentIndex(index);
    }

    //! При переходе на другую ленту метим старую просмотренной
    setFeedRead(feedIdOld, 0);

    slotFeedsTreeSelected(index, true);
    feedsView_->repaint();
  }
  feedIdOld = feedsModel_->index(
      feedsView_->currentIndex().row(), feedsModel_->fieldIndex("id")).data().toInt();

  if (indexTab != -1) {
    tabWidget_->setCurrentIndex(indexTab);
  }
}

void RSSListing::slotFeedClicked(QModelIndex index)
{
  int feedIdCur = feedsTreeModel_->index(
      index.row(), feedsTreeView_->columnIndex("id"), index.parent()).
      data().toInt();

  // Поиск уже открытого таба с этой лентой
  int indexTab = -1;
  for (int i = 0; i < tabWidget_->count(); i++) {
    NewsTabWidget *widget = (NewsTabWidget*)tabWidget_->widget(i);
    if (widget->feedId_ == feedIdCur) {
      indexTab = i;
      break;
    }
  }

  qDebug() << feedIdCur << feedIdOld << indexTab;

  if ((feedIdCur != feedIdOld) || (indexTab == -1)) {
    if (tabWidget_->currentIndex() != 0) {
      tabWidget_->setCurrentIndex(0);
      feedsView_->setCurrentIndex(index);
    }

    //! При переходе на другую ленту метим старую просмотренной
    setFeedRead(feedIdOld, 0);

//    slotFeedsTreeSelected(index, true);
    feedsView_->repaint();
  }
  feedIdOld = feedIdCur;

  if (indexTab != -1) {
    tabWidget_->setCurrentIndex(indexTab);
  }
}

void RSSListing::slotFeedsTreeSelected(QModelIndex index, bool clicked,
                                       bool createTab)
{
  QElapsedTimer timer;
  timer.start();
  qDebug() << "--------------------------------";
  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();

  int feedRow = index.row();

  if ((!tabWidget_->count() && clicked) || createTab) {
    int feedId = feedsModel_->index(feedRow, feedsModel_->fieldIndex("id")).data().toInt();
    int indexTab = tabWidget_->addTab(
          new NewsTabWidget(feedId, this), "");
    createNewsTab(indexTab);

    tabBar_->setTabButton(indexTab,
                          QTabBar::LeftSide,
                          currentNewsTab->newsTitleLabel_);
    if (indexTab == 0)
      currentNewsTab->closeButton_->setVisible(false);

    emit signalCurrentTab(indexTab, true);
  } else {
    currentNewsTab->feedId_ = feedsModel_->index(feedRow, feedsModel_->fieldIndex("id")).data().toInt();
    currentNewsTab->setSettings(false);
    if (index.isValid())
      currentNewsTab->setVisible(true);
  }

  //! Устанавливаем иконку и текст для открытой вкладки
  QPixmap iconTab;
  QByteArray byteArray = feedsModel_->index(
        feedRow, feedsModel_->fieldIndex("image")).data().toByteArray();
  if (!byteArray.isNull()) {
    iconTab.loadFromData(QByteArray::fromBase64(byteArray));
  } else {
    iconTab.load(":/images/feed");
  }
  currentNewsTab->newsIconTitle_->setPixmap(iconTab);

  QString tabText = feedsModel_->index(feedRow, feedsModel_->fieldIndex("text")).data().toString();
  currentNewsTab->newsTitleLabel_->setToolTip(tabText);
  tabText = currentNewsTab->newsTextTitle_->fontMetrics().elidedText(
        tabText, Qt::ElideRight, 114);
  currentNewsTab->newsTextTitle_->setText(tabText);

  feedProperties_->setEnabled(index.isValid());
  if (!index.isValid())
    currentNewsTab->setVisible(false);

  setFeedsFilter(feedsFilterGroup_->checkedAction(), false);

  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();

  setNewsFilter(newsFilterGroup_->checkedAction(), false);

  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();

  if (newsModel_->rowCount() != 0) {
    while (newsModel_->canFetchMore())
      newsModel_->fetchMore();
  }

  // выбор новости ленты, отображамой ранее
  int newsRow = -1;
  if ((openingFeedAction_ == 0) || !clicked) {
    for (int i = 0; i < newsModel_->rowCount(); i++) {
      if (newsModel_->index(i, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() ==
          feedsModel_->index(feedRow, feedsModel_->fieldIndex("currentNews")).data().toInt()) {
        newsRow = i;
        break;
      }
    }
  } else if (openingFeedAction_ == 1) {
    newsRow = 0;
  } else if (openingFeedAction_ == 3) {
    for (int i = newsModel_->rowCount()-1; i >= 0; i--) {
      if (newsModel_->index(i, newsModel_->fieldIndex("read")).data(Qt::EditRole).toInt() == 0) {
        newsRow = i;
        break;
      }
    }
  }

  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();

  newsView_->setCurrentIndex(newsModel_->index(newsRow, newsModel_->fieldIndex("title")));

  if (newsRow == -1)
    newsView_->verticalScrollBar()->setValue(newsRow);

  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();

  if (clicked) {
    if ((openingFeedAction_ != 2) && openNewsWebViewOn_) {
      currentNewsTab->slotNewsViewSelected(newsModel_->index(newsRow, newsModel_->fieldIndex("title")));
      qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();
    } else {
      currentNewsTab->slotNewsViewSelected(newsModel_->index(-1, 6));
      qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();
      QSqlQuery q(db_);
      int newsId = newsModel_->index(newsRow, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();
      int feedId = feedsModel_->index(feedRow, feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();
      QString qStr = QString("UPDATE feeds SET currentNews='%1' WHERE id=='%2'").arg(newsId).arg(feedId);
      q.exec(qStr);
      qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();
    }
  } else {
    slotUpdateStatus();
    qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();
  }
}

/*! \brief Вызов окна настроек ************************************************/
void RSSListing::showOptionDlg()
{
  static int index = 0;
  OptionsDialog *optionsDialog = new OptionsDialog(this);
  optionsDialog->restoreGeometry(settings_->value("options/geometry").toByteArray());
  optionsDialog->setCurrentItem(index);

  optionsDialog->showSplashScreen_->setChecked(showSplashScreen_);
  optionsDialog->reopenFeedStartup_->setChecked(reopenFeedStartup_);

  optionsDialog->storeDBMemory_->setChecked(storeDBMemoryT_);

  optionsDialog->showTrayIconBox_->setChecked(showTrayIcon_);
  optionsDialog->startingTray_->setChecked(startingTray_);
  optionsDialog->minimizingTray_->setChecked(minimizingTray_);
  optionsDialog->closingTray_->setChecked(closingTray_);
  optionsDialog->setBehaviorIconTray(behaviorIconTray_);
  optionsDialog->singleClickTray_->setChecked(singleClickTray_);
  optionsDialog->clearStatusNew_->setChecked(clearStatusNew_);
  optionsDialog->emptyWorking_->setChecked(emptyWorking_);

  optionsDialog->setProxy(networkProxy_);

  optionsDialog->embeddedBrowserOn_->setChecked(externalBrowserOn_ == 0);
  optionsDialog->standartBrowserOn_->setChecked(externalBrowserOn_ == 1);
  optionsDialog->editExternalBrowser_->setText(externalBrowser_);
  optionsDialog->javaScriptEnable_->setChecked(javaScriptEnable_);
  optionsDialog->pluginsEnable_->setChecked(pluginsEnable_);
  optionsDialog->openLinkInBackground_->setChecked(openLinkInBackground_);

  optionsDialog->updateFeedsStartUp_->setChecked(autoUpdatefeedsStartUp_);
  optionsDialog->updateFeeds_->setChecked(autoUpdatefeeds_);
  optionsDialog->intervalTime_->setCurrentIndex(autoUpdatefeedsInterval_);
  optionsDialog->updateFeedsTime_->setValue(autoUpdatefeedsTime_);

  optionsDialog->setOpeningFeed(openingFeedAction_);
  optionsDialog->openNewsWebViewOn_->setChecked(openNewsWebViewOn_);

  optionsDialog->markNewsReadOn_->setChecked(markNewsReadOn_);
  optionsDialog->markNewsReadTime_->setValue(markNewsReadTime_);
  optionsDialog->markReadSwitchingFeed_->setChecked(markReadSwitchingFeed_);
  optionsDialog->markReadClosingTab_->setChecked(markReadClosingTab_);
  optionsDialog->markReadMinimize_->setChecked(markReadMinimize_);

  optionsDialog->showDescriptionNews_->setChecked(showDescriptionNews_);

  for (int i = 0; i < optionsDialog->formatDateTime_->count(); i++) {
    if (optionsDialog->formatDateTime_->itemData(i).toString() == formatDateTime_) {
      optionsDialog->formatDateTime_->setCurrentIndex(i);
      break;
    }
  }

  optionsDialog->dayCleanUpOn_->setChecked(dayCleanUpOn_);
  optionsDialog->maxDayCleanUp_->setValue(maxDayCleanUp_);
  optionsDialog->newsCleanUpOn_->setChecked(newsCleanUpOn_);
  optionsDialog->maxNewsCleanUp_->setValue(maxNewsCleanUp_);
  optionsDialog->readCleanUp_->setChecked(readCleanUp_);
  optionsDialog->neverUnreadCleanUp_->setChecked(neverUnreadCleanUp_);
  optionsDialog->neverStarCleanUp_->setChecked(neverStarCleanUp_);

  optionsDialog->soundNewNews_->setChecked(soundNewNews_);
  optionsDialog->editSoundNotifer_->setText(soundNotifyPath_);
  optionsDialog->showNotifyOn_->setChecked(showNotifyOn_);
  optionsDialog->countShowNewsNotify_->setValue(countShowNewsNotify_);
  optionsDialog->widthTitleNewsNotify_->setValue(widthTitleNewsNotify_);
  optionsDialog->timeShowNewsNotify_->setValue(timeShowNewsNotify_);
  optionsDialog->onlySelectedFeeds_->setChecked(onlySelectedFeeds_);

  QSqlQuery q(db_);
  db_.transaction();
  QString qStr = QString("SELECT text, id, image FROM feeds");
  q.exec(qStr);
  while (q.next()) {
    QStringList treeItem;
    treeItem << q.value(0).toString() << q.value(1).toString();
    QTreeWidgetItem *treeWidgetItem = new QTreeWidgetItem(treeItem);

    QPixmap iconTab;
    QByteArray byteArray = q.value(2).toByteArray();
    if (!byteArray.isNull()) {
      iconTab.loadFromData(QByteArray::fromBase64(byteArray));
    } else {
      iconTab.load(":/images/feed");
    }
    treeWidgetItem->setIcon(0, iconTab);

    optionsDialog->feedsTreeNotify_->topLevelItem(0)->addChild(treeWidgetItem);
    treeWidgetItem->setCheckState(0, Qt::Unchecked);

    QSqlQuery q1(db_);
    qStr = QString("SELECT value FROM feeds_ex WHERE feedId='%1' AND name='showNotification'").
        arg(q.value(1).toInt());
    q1.exec(qStr);
    if (q1.next()) {
      if (q1.value(0).toInt() == 1)
        treeWidgetItem->setCheckState(0, Qt::Checked);
    } else {
      qStr = QString("INSERT INTO feeds_ex(feedId, name, value) VALUES ('%1', 'showNotification', '0')").
          arg(q.value(1).toInt());
      q1.exec(qStr);
    }
  }
  db_.commit();
  optionsDialog->feedsTreeNotify_->expandAll();

  optionsDialog->setLanguage(langFileName_);

  QString strFont = QString("%1, %2").
      arg(feedsView_->font().family()).
      arg(feedsView_->font().pointSize());
  optionsDialog->fontTree->topLevelItem(0)->setText(2, strFont);
  strFont = QString("%1, %2").arg(newsFontFamily_).arg(newsFontSize_);
  optionsDialog->fontTree->topLevelItem(1)->setText(2, strFont);
  strFont = QString("%1, %2").arg(panelNewsFontFamily_).arg(panelNewsFontSize_);
  optionsDialog->fontTree->topLevelItem(2)->setText(2, strFont);
  strFont = QString("%1, %2").arg(webFontFamily_).arg(webFontSize_);
  optionsDialog->fontTree->topLevelItem(3)->setText(2, strFont);
  strFont = QString("%1, %2").arg(notificationFontFamily_).arg(notificationFontSize_);
  optionsDialog->fontTree->topLevelItem(4)->setText(2, strFont);

  optionsDialog->loadActionShortcut(listActions_, &listDefaultShortcut_);
//
  int result = optionsDialog->exec();
  settings_->setValue("options/geometry", optionsDialog->saveGeometry());
  index = optionsDialog->currentIndex();

  if (result == QDialog::Rejected) {
    delete optionsDialog;
    return;
  }

  optionsDialog->saveActionShortcut(listActions_);

  showSplashScreen_ = optionsDialog->showSplashScreen_->isChecked();
  reopenFeedStartup_ = optionsDialog->reopenFeedStartup_->isChecked();

  storeDBMemoryT_ = optionsDialog->storeDBMemory_->isChecked();

  showTrayIcon_ = optionsDialog->showTrayIconBox_->isChecked();
  startingTray_ = optionsDialog->startingTray_->isChecked();
  minimizingTray_ = optionsDialog->minimizingTray_->isChecked();
  closingTray_ = optionsDialog->closingTray_->isChecked();
  behaviorIconTray_ = optionsDialog->behaviorIconTray();
  if (behaviorIconTray_ > CHANGE_ICON_TRAY) {
    refreshInfoTray();
  } else {
#if defined(QT_NO_DEBUG_OUTPUT)
    traySystem->setIcon(QIcon(":/images/quiterss16"));
#else
    traySystem->setIcon(QIcon(":/images/quiterssDebug"));
#endif
  }
  singleClickTray_ = optionsDialog->singleClickTray_->isChecked();
  clearStatusNew_ = optionsDialog->clearStatusNew_->isChecked();
  emptyWorking_ = optionsDialog->emptyWorking_->isChecked();
  if (showTrayIcon_) traySystem->show();
  else traySystem->hide();

  networkProxy_ = optionsDialog->proxy();
  persistentUpdateThread_->setProxy(networkProxy_);

  if (optionsDialog->embeddedBrowserOn_->isChecked())
    externalBrowserOn_ = 0;
  else if (optionsDialog->externalBrowserOn_->isChecked())
    externalBrowserOn_ = 2;
  else
    externalBrowserOn_ = 1;

  if (!externalBrowserOn_) {
    openInExternalBrowserAct_->setVisible(true);
    openNewsNewTabAct_->setVisible(true);
    openNewsBackgroundTabAct_->setVisible(true);
  } else {
    QList <QKeySequence> keySequenceList;
    keySequenceList << openInBrowserAct_->shortcut()
                    << openInExternalBrowserAct_->shortcut();
    openInBrowserAct_->setShortcuts(keySequenceList);
    openInExternalBrowserAct_->setVisible(false);
    openNewsNewTabAct_->setVisible(false);
    openNewsBackgroundTabAct_->setVisible(false);
  }
  externalBrowser_ = optionsDialog->editExternalBrowser_->text();
  javaScriptEnable_ = optionsDialog->javaScriptEnable_->isChecked();
  pluginsEnable_ = optionsDialog->pluginsEnable_->isChecked();
  openLinkInBackground_ = optionsDialog->openLinkInBackground_->isChecked();

  autoUpdatefeedsStartUp_ = optionsDialog->updateFeedsStartUp_->isChecked();
  autoUpdatefeeds_ = optionsDialog->updateFeeds_->isChecked();
  autoUpdatefeedsTime_ = optionsDialog->updateFeedsTime_->value();
  autoUpdatefeedsInterval_ = optionsDialog->intervalTime_->currentIndex();
  if (updateFeedsTimer_.isActive() && !autoUpdatefeeds_) {
    updateFeedsTimer_.stop();
  } else if (!updateFeedsTimer_.isActive() && autoUpdatefeeds_) {
    int updateFeedsTime = autoUpdatefeedsTime_*60000;
    if (autoUpdatefeedsInterval_ == 1)
      updateFeedsTime = updateFeedsTime*60;
    updateFeedsTimer_.start(updateFeedsTime, this);
  }

  openingFeedAction_ = optionsDialog->getOpeningFeed();
  openNewsWebViewOn_ = optionsDialog->openNewsWebViewOn_->isChecked();

  markNewsReadOn_ = optionsDialog->markNewsReadOn_->isChecked();
  markNewsReadTime_ = optionsDialog->markNewsReadTime_->value();
  markReadSwitchingFeed_ = optionsDialog->markReadSwitchingFeed_->isChecked();
  markReadClosingTab_ = optionsDialog->markReadClosingTab_->isChecked();
  markReadMinimize_ = optionsDialog->markReadMinimize_->isChecked();

  showDescriptionNews_ = optionsDialog->showDescriptionNews_->isChecked();

  formatDateTime_ = optionsDialog->formatDateTime_->itemData(
        optionsDialog->formatDateTime_->currentIndex()).toString();
  feedsModel_->formatDateTime_ = formatDateTime_;

  dayCleanUpOn_ = optionsDialog->dayCleanUpOn_->isChecked();
  maxDayCleanUp_ = optionsDialog->maxDayCleanUp_->value();
  newsCleanUpOn_ = optionsDialog->newsCleanUpOn_->isChecked();
  maxNewsCleanUp_ = optionsDialog->maxNewsCleanUp_->value();
  readCleanUp_ = optionsDialog->readCleanUp_->isChecked();
  neverUnreadCleanUp_ = optionsDialog->neverUnreadCleanUp_->isChecked();
  neverStarCleanUp_ = optionsDialog->neverStarCleanUp_->isChecked();

  soundNewNews_ = optionsDialog->soundNewNews_->isChecked();
  soundNotifyPath_ = optionsDialog->editSoundNotifer_->text();
  showNotifyOn_ = optionsDialog->showNotifyOn_->isChecked();
  countShowNewsNotify_ = optionsDialog->countShowNewsNotify_->value();
  widthTitleNewsNotify_ = optionsDialog->widthTitleNewsNotify_->value();
  timeShowNewsNotify_ = optionsDialog->timeShowNewsNotify_->value();
  onlySelectedFeeds_ = optionsDialog->onlySelectedFeeds_->isChecked();

  QTreeWidgetItem *treeWidgetItem =
      optionsDialog->feedsTreeNotify_->topLevelItem(0);
  db_.transaction();
  for (int i = 0; i < treeWidgetItem->childCount(); i++) {
    int check = 0;
    if (treeWidgetItem->child(i)->checkState(0) == Qt::Checked)
      check = 1;

    qStr = QString("UPDATE feeds_ex SET value='%1' WHERE feedId='%2' AND name='showNotification'").
        arg(check).arg(treeWidgetItem->child(i)->text(1).toInt());
    q.exec(qStr);
  }
  db_.commit();

  if (!langFileName_.contains(optionsDialog->language(), Qt::CaseInsensitive)) {
    langFileName_ = optionsDialog->language();
    appInstallTranslator();
  }

  QFont font = feedsView_->font();
  font.setFamily(
        optionsDialog->fontTree->topLevelItem(0)->text(2).section(", ", 0, 0));
  font.setPointSize(
        optionsDialog->fontTree->topLevelItem(0)->text(2).section(", ", 1).toInt());
  feedsView_->setFont(font);
  feedsModel_->font_ = font;

  newsFontFamily_ = optionsDialog->fontTree->topLevelItem(1)->text(2).section(", ", 0, 0);
  newsFontSize_ = optionsDialog->fontTree->topLevelItem(1)->text(2).section(", ", 1).toInt();
  panelNewsFontFamily_ = optionsDialog->fontTree->topLevelItem(2)->text(2).section(", ", 0, 0);
  panelNewsFontSize_ = optionsDialog->fontTree->topLevelItem(2)->text(2).section(", ", 1).toInt();
  webFontFamily_ = optionsDialog->fontTree->topLevelItem(3)->text(2).section(", ", 0, 0);
  webFontSize_ = optionsDialog->fontTree->topLevelItem(3)->text(2).section(", ", 1).toInt();
  notificationFontFamily_ = optionsDialog->fontTree->topLevelItem(4)->text(2).section(", ", 0, 0);
  notificationFontSize_ = optionsDialog->fontTree->topLevelItem(4)->text(2).section(", ", 1).toInt();

  delete optionsDialog;

  if (currentNewsTab != NULL)
    currentNewsTab->setSettings();

  writeSettings();
  saveActionShortcuts();
}

/*! \brief Создание меню трея *************************************************/
void RSSListing::createTrayMenu()
{
  trayMenu_ = new QMenu(this);
  showWindowAct_ = new QAction(this);
  connect(showWindowAct_, SIGNAL(triggered()), this, SLOT(slotShowWindows()));
  QFont font_ = showWindowAct_->font();
  font_.setBold(true);
  showWindowAct_->setFont(font_);
  trayMenu_->addAction(showWindowAct_);
  trayMenu_->addAction(updateAllFeedsAct_);
  trayMenu_->addSeparator();

  trayMenu_->addAction(optionsAct_);
  trayMenu_->addSeparator();

  trayMenu_->addAction(exitAct_);
  traySystem->setContextMenu(trayMenu_);
}

/*! \brief Освобождение памяти ************************************************/
void RSSListing::myEmptyWorkingSet()
{
#if defined(Q_WS_WIN)
  if (isHidden())
    EmptyWorkingSet(GetCurrentProcess());
#endif
}

/*! \brief Показ статус бара после запрос обновления ленты ********************/
void RSSListing::showProgressBar(int addToMaximum)
{
  progressBar_->setMaximum(progressBar_->maximum() + addToMaximum);
  updateFeedsCount_ = addToMaximum;
  idFeedList_.clear();
  cntNewNewsList_.clear();
  showMessageOn_ = true;
  statusBar()->showMessage(progressBar_->text());
  progressBar_->show();
  QTimer::singleShot(150, this, SLOT(slotProgressBarUpdate()));
  emit startGetUrlTimer();
}

/*! \brief Обновление ленты (действие) ****************************************/
void RSSListing::slotGetFeed()
{
  playSoundNewNews_ = false;

  persistentUpdateThread_->requestUrl(
        feedsModel_->record(feedsView_->selectIndex.row()).field("xmlUrl").value().toString(),
        QDateTime::fromString(feedsModel_->record(feedsView_->selectIndex.row()).field("lastBuildDate").value().toString(), Qt::ISODate)
        );
  showProgressBar(1);
}

/*! \brief Обновление ленты (действие) ****************************************/
void RSSListing::slotGetAllFeeds()
{
  int feedCount = 0;

  playSoundNewNews_ = false;

  QSqlQuery q(db_);
  q.exec("select xmlUrl, lastBuildDate from feeds where xmlUrl is not null");
  qDebug() << q.lastError();
  while (q.next()) {
    persistentUpdateThread_->requestUrl(q.record().value(0).toString(),
                                        q.record().value(1).toDateTime());
    ++feedCount;
  }

  if (feedCount) {
    updateAllFeedsAct_->setEnabled(false);
    updateFeedAct_->setEnabled(false);
    showProgressBar(feedCount);
  }
}

void RSSListing::slotProgressBarUpdate()
{
  progressBar_->update();

  if (progressBar_->isVisible())
    QTimer::singleShot(150, this, SLOT(slotProgressBarUpdate()));
}

void RSSListing::slotVisibledFeedsDock()
{
  feedsDock_->setVisible(!feedsDock_->isVisible());
}

void RSSListing::updateIconToolBarNull(bool feedsDockVisible)
{
  if (feedsDockArea_ == Qt::LeftDockWidgetArea) {
    if (feedsDockVisible)
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleR.png"));
    else
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleL.png"));
  } else if (feedsDockArea_ == Qt::RightDockWidgetArea) {
    if (feedsDockVisible)
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleL.png"));
    else
      pushButtonNull_->setIcon(QIcon(":/images/images/triangleR.png"));
  }
}

void RSSListing::slotDockLocationChanged(Qt::DockWidgetArea area)
{
  if (area == Qt::LeftDockWidgetArea) {
    toolBarNull_->show();
    addToolBar(Qt::LeftToolBarArea, toolBarNull_);
  } else if (area == Qt::RightDockWidgetArea) {
    toolBarNull_->show();
    addToolBar(Qt::RightToolBarArea, toolBarNull_);
  } else {
    toolBarNull_->hide();
  }
  updateIconToolBarNull(feedsDock_->isVisible());
}

void RSSListing::markFeedRead()
{
  int feedId = feedsModel_->index(
            feedsView_->selectIndex.row(),
            feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();

  db_.transaction();
  QSqlQuery q(db_);
  if (currentNewsTab->feedId_ == feedId) {
    QString qStr = QString("UPDATE news SET read=2 WHERE feedId='%1' AND read!=2").
        arg(feedId);
    q.exec(qStr);
    qStr = QString("UPDATE news SET new=0 WHERE feedId='%1' AND new=1").
        arg(feedId);
    q.exec(qStr);
    qStr = QString("UPDATE feeds SET newCount=0, unread=0 WHERE id='%1'").
        arg(feedId);
    q.exec(qStr);
  } else {
    QString qStr = QString("UPDATE news SET read=1 WHERE feedId='%1' AND read=0").
        arg(feedId);
    q.exec(qStr);
    qStr = QString("UPDATE news SET new=0 WHERE feedId='%1' AND new=1").
        arg(feedId);
    q.exec(qStr);
  }
  db_.commit();

  if (currentNewsTab->feedId_ == feedId) {
    if (tabWidget_->currentIndex() == 0) {
      int row = feedsView_->currentIndex().row();
      if ((row+1) == feedsModel_->rowCount()) row--;
      else row++;
      feedsView_->setCurrentIndex(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
      slotFeedsTreeClicked(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
    } else {
      int currentRow = newsView_->currentIndex().row();

      newsModel_->select();

      while (newsModel_->canFetchMore())
        newsModel_->fetchMore();

      newsView_->setCurrentIndex(newsModel_->index(currentRow, newsModel_->fieldIndex("title")));

      slotUpdateStatus();
    }
  } else {
    slotUpdateStatus(false);
  }
}

/*! \brief Подсчёт новостей
 *
 * Подсчёт всех новостей в фиде. (50мс)
 * Подсчёт всех не прочитанных новостей в фиде. (50мс)
 * Подсчёт всех новых новостей в фиде. (50мс)
 * Запись этих данных в таблицу лент (100мс)
 * Вывод этих данных в статусную строку
 */
void RSSListing::slotUpdateStatus(bool openFeed)
{
  QSqlQuery q(db_);
  QString qStr;

  int feedId;
  if (feedsView_->selectIndex.isValid())
    feedId = feedsModel_->index(
            feedsView_->selectIndex.row(),
            feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();
  else
    feedId = currentNewsTab->feedId_;

  int newCountOld = 0;
  qStr = QString("SELECT newCount FROM feeds WHERE id=='%1'").
      arg(feedId);
  q.exec(qStr);
  if (q.next()) newCountOld = q.value(0).toInt();

  recountFeedCounts(feedId, feedsView_->currentIndex());

  int newCount = 0;
  int unreadCount = 0;
  int allCount = 0;
  qStr = QString("SELECT newCount, unread, undeleteCount FROM feeds WHERE id=='%1'").
      arg(feedId);
  q.exec(qStr);
  if (q.next()) {
    newCount    = q.value(0).toInt();
    unreadCount = q.value(1).toInt();
    allCount    = q.value(2).toInt();
  }

  if (!isActiveWindow() && (newCount > newCountOld) &&
      (behaviorIconTray_ == CHANGE_ICON_TRAY)) {
    traySystem->setIcon(QIcon(":/images/quiterss16_NewNews"));
  }
  refreshInfoTray();
  if (newCount > newCountOld) {
    playSoundNewNews();
  }

  feedsModelReload();

  if (openFeed) {
    statusUnread_->setText(QString(tr(" Unread: %1 ")).arg(unreadCount));
    statusAll_->setText(QString(tr(" All: %1 ")).arg(allCount));
  }
  if (feedId > 0) {
    statusUnread_->setVisible(true);
    statusAll_->setVisible(true);
  } else {
    statusUnread_->setVisible(false);
    statusAll_->setVisible(false);
  }
}

void RSSListing::setFeedsFilter(QAction* pAct, bool clicked)
{
  int id = feedsModel_->index(
        feedsView_->currentIndex().row(),
        feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();
  int newCount = feedsModel_->index(
        feedsView_->currentIndex().row(),
        feedsModel_->fieldIndex("newCount")).data(Qt::EditRole).toInt();
  int unRead = feedsModel_->index(
        feedsView_->currentIndex().row(),
        feedsModel_->fieldIndex("unread")).data(Qt::EditRole).toInt();

  QString strFilter;
  if (pAct->objectName() == "filterFeedsAll_") {
    strFilter = "";
  } else if (pAct->objectName() == "filterFeedsNew_") {
    if (clicked && !newCount) {
      strFilter = QString("newCount > 0");
    } else
      strFilter = QString("(newCount > 0 OR id=='%1')").arg(id);
  } else if (pAct->objectName() == "filterFeedsUnread_") {
    if (clicked && !unRead) {
      strFilter = QString("unread > 0");
    } else
      strFilter = QString("(unread > 0 OR id=='%1')").arg(id);
  } else if (pAct->objectName() == "filterFeedsStarred_") {
    strFilter = QString("label LIKE '\%starred\%'");
  }

  if (findFeedsWidget_->isVisible()) {
    if (pAct->objectName() != "filterFeedsAll_")
      strFilter.append(" AND ");
    if (findFeeds_->findGroup_->checkedAction()->objectName() == "findNameAct") {
      strFilter.append(QString("text LIKE '\%%1\%'").arg(findFeeds_->text()));
    } else {
      strFilter.append(QString("xmlUrl LIKE '\%%1\%'").arg(findFeeds_->text()));
    }
  }

  feedsModel_->setFilter(strFilter);

  if (pAct->objectName() == "filterFeedsAll_") feedsFilter_->setIcon(QIcon(":/images/filterOff"));
  else feedsFilter_->setIcon(QIcon(":/images/filterOn"));

  int rowFeeds = -1;
  for (int i = 0; i < feedsModel_->rowCount(); i++) {
    if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() == id) {
      rowFeeds = i;
    }
  }
  feedsView_->updateCurrentIndex(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));

  if (clicked && (tabWidget_->currentIndex() == 0)) {
    slotFeedsTreeClicked(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));
  }

  if (pAct->objectName() != "filterFeedsAll_")
    feedsFilterAction = pAct;
}

void RSSListing::setNewsFilter(QAction* pAct, bool clicked)
{
  if (currentNewsTab == NULL) return;
  QElapsedTimer timer;
  timer.start();
  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();

  QModelIndex index = newsView_->currentIndex();

  int feedId = currentNewsTab->feedId_;
  int newsId = newsModel_->index(
        index.row(), newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();

  if (clicked) {
    QString qStr = QString("UPDATE news SET read=2 WHERE feedId='%1' AND read=1").
        arg(feedId);
    QSqlQuery q(db_);
    q.exec(qStr);
  }

  newsFilterStr = QString("feedId=%1 AND ").arg(feedId);

  if (pAct->objectName() == "filterNewsAll_") {
    newsFilterStr.append("deleted = 0");
  } else if (pAct->objectName() == "filterNewsNew_") {
    newsFilterStr.append(QString("new = 1 AND deleted = 0"));
  } else if (pAct->objectName() == "filterNewsUnread_") {
    newsFilterStr.append(QString("read < 2 AND deleted = 0"));
  } else if (pAct->objectName() == "filterNewsStar_") {
    newsFilterStr.append(QString("starred = 1 AND deleted = 0"));
  } else if (pAct->objectName() == "filterNewsNotStarred_") {
    newsFilterStr.append(QString("starred = 0 AND deleted = 0"));
  } else if (pAct->objectName() == "filterNewsUnreadStar_") {
    newsFilterStr.append(QString("(read < 2 OR starred = 1) AND deleted = 0"));
  } else if (pAct->objectName() == "filterNewsDeleted_") {
    newsFilterStr.append(QString("deleted = 1"));
  }

  if (pAct->objectName() == "filterNewsDeleted_")
    currentNewsTab->setVisibleAction(true);
  else
    currentNewsTab->setVisibleAction(false);

  QString filterStr = newsFilterStr;
  if (currentNewsTab->findText_->findGroup_->checkedAction()->objectName() == "findInNewsAct") {
    filterStr.append(
        QString(" AND (title LIKE '\%%1\%' OR author_name LIKE '\%%1\%' OR category LIKE '\%%1\%')").
        arg(currentNewsTab->findText_->text()));
  }

  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed() << filterStr;
  newsModel_->setFilter(filterStr);
  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();

  if ((currentNewsTab->newsHeader_->sortIndicatorSection() == newsModel_->fieldIndex("read")) ||
      currentNewsTab->newsHeader_->sortIndicatorSection() == newsModel_->fieldIndex("starred")) {
    currentNewsTab->slotSort(currentNewsTab->newsHeader_->sortIndicatorSection(),
                             currentNewsTab->newsHeader_->sortIndicatorOrder());
  }

  if (pAct->objectName() == "filterNewsAll_") newsFilter_->setIcon(QIcon(":/images/filterOff"));
  else newsFilter_->setIcon(QIcon(":/images/filterOn"));

  if (clicked) {
    int newsRow = -1;
    for (int i = 0; i < newsModel_->rowCount(); i++) {
      if (newsModel_->index(i, newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() == newsId) {
        newsRow = i;
      }
    }
    newsView_->setCurrentIndex(newsModel_->index(newsRow, newsModel_->fieldIndex("title")));
    if (newsRow == -1)
      currentNewsTab->hideWebContent();
  }

  qDebug() << __FUNCTION__ << __LINE__ << timer.elapsed();

  if (pAct->objectName() != "filterNewsAll_")
    newsFilterAction = pAct;
}

void RSSListing::slotFeedsDockLocationChanged(Qt::DockWidgetArea area)
{
  feedsDockArea_ = area;
}

//! Маркировка ленты прочитанной при клике на не отмеченной ленте
void RSSListing::setFeedRead(int feedId, int type)
{
  if (feedId <= -1) return;

  bool update = false;
  db_.transaction();
  QSqlQuery q(db_);
  if ((markReadSwitchingFeed_ && (type == 0)) ||
      (markReadClosingTab_ && (type == 1)) ||
      (markReadMinimize_ && (type == 2))) {
    q.exec(QString("UPDATE news SET read=2 WHERE feedId='%1'").arg(feedId));
    update = true;
  }
  else
    q.exec(QString("UPDATE news SET read=2 WHERE feedId='%1' AND read=1").arg(feedId));
  q.exec(QString("UPDATE news SET new=0 WHERE feedId='%1' AND new=1").arg(feedId));

  q.exec(QString("UPDATE feeds SET newCount=0 WHERE id='%1'").arg(feedId));
  db_.commit();

  if (update) {
    recountFeedCounts(feedId);
    if (type != 2) {
      refreshInfoTray();
      feedsModelReload();
    }
  }
}

void RSSListing::slotShowAboutDlg()
{
  AboutDialog *aboutDialog = new AboutDialog(langFileName_, this);
  aboutDialog->exec();
  delete aboutDialog;
}

void RSSListing::createMenuFeed()
{
  feedContextMenu_ = new QMenu(this);
  feedContextMenu_->addAction(addFeedAct_);
  feedContextMenu_->addAction(addFolderAct_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(openFeedNewTabAct_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(updateFeedAct_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(markFeedRead_);
//  feedContextMenu_->addAction(markAllFeedsRead_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(deleteFeedAct_);
  feedContextMenu_->addSeparator();
  feedContextMenu_->addAction(setFilterNewsAct_);
  feedContextMenu_->addAction(feedProperties_);

  connect(feedContextMenu_, SIGNAL(aboutToHide()),
          feedsView_, SLOT(setSelectIndex()), Qt::QueuedConnection);
  connect(feedContextMenu_, SIGNAL(aboutToShow()),
          this, SLOT(slotFeedMenuShow()));
}

void RSSListing::showContextMenuFeed(const QPoint &p)
{
  if (feedsView_->indexAt(p).isValid())
    feedContextMenu_->popup(feedsView_->viewport()->mapToGlobal(p));
}

void RSSListing::setAutoLoadImages(bool set)
{
  autoLoadImages_ = !autoLoadImages_;
  if (autoLoadImages_) {
    autoLoadImagesToggle_->setText(tr("Load Images"));
    autoLoadImagesToggle_->setToolTip(tr("Auto Load Images to News View"));
    autoLoadImagesToggle_->setIcon(QIcon(":/images/imagesOn"));
  } else {
    autoLoadImagesToggle_->setText(tr("No Load Images"));
    autoLoadImagesToggle_->setToolTip(tr("No Load Images to News View"));
    autoLoadImagesToggle_->setIcon(QIcon(":/images/imagesOff"));
  }

  if (newsView_ && set) {
    NewsTabWidget *widget = qobject_cast<NewsTabWidget*>(tabWidget_->currentWidget());
    widget->autoLoadImages_ = autoLoadImages_;
    widget->webView_->settings()->setAttribute(
          QWebSettings::AutoLoadImages, autoLoadImages_);
    if (autoLoadImages_) {
      if ((widget->webView_->history()->count() == 0) && (widget->feedId_ > -1))
        currentNewsTab->updateWebView(newsView_->currentIndex());
      else widget->webView_->reload();
    }
  }
}

void RSSListing::loadSettingsFeeds()
{
  markNewsReadOn_ = false;
  behaviorIconTray_ = settings_->value("Settings/behaviorIconTray", NEW_COUNT_ICON_TRAY).toInt();

  QString filterName = settings_->value("feedSettings/filterName", "filterFeedsAll_").toString();
  QList<QAction*> listActions = feedsFilterGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == filterName) {
      action->setChecked(true);
      break;
    }
  }
  filterName = settings_->value("newsSettings/filterName", "filterNewsAll_").toString();
  listActions = newsFilterGroup_->actions();
  foreach(QAction *action, listActions) {
    if (action->objectName() == filterName) {
      action->setChecked(true);
      break;
    }
  }

  setFeedsFilter(feedsFilterGroup_->checkedAction(), false);
}

void RSSListing::setCurrentFeed()
{
  qApp->processEvents();

  int row = -1;
  if (reopenFeedStartup_) {
    int id = settings_->value("feedSettings/currentId", 0).toInt();
    for (int i = 0; i < feedsModel_->rowCount(); i++) {
      if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data().toInt() == id) {
        row = i;
        break;
      }
    }
  }

  feedsView_->setCurrentIndex(feedsModel_->index(row, feedsModel_->fieldIndex("text"))); // загрузка новостей
  tabCurrentUpdateOff_ = true;
  slotFeedsTreeClicked(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
  tabCurrentUpdateOff_ = false;

  QSqlQuery q(db_);
  q.exec(QString("SELECT id FROM feeds WHERE displayOnStartup=1"));
  while(q.next()) {
    creatFeedTab(q.value(0).toInt());
  }
}

void RSSListing::slotFeedsFilter()
{
  if (feedsFilterGroup_->checkedAction()->objectName() == "filterFeedsAll_") {
    if (feedsFilterAction != NULL) {
      feedsFilterAction->setChecked(true);
      setFeedsFilter(feedsFilterAction);
    } else {
      feedsFilterMenu_->popup(
            feedsToolBar_->mapToGlobal(QPoint(0, feedsToolBar_->height()-1)));
    }
  } else {
    filterFeedsAll_->setChecked(true);
    setFeedsFilter(filterFeedsAll_);
  }
}

void RSSListing::slotNewsFilter()
{
  if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsAll_") {
    if (newsFilterAction != NULL) {
      newsFilterAction->setChecked(true);
      setNewsFilter(newsFilterAction);
    } else {
      newsFilterMenu_->popup(
            currentNewsTab->newsToolBar_->mapToGlobal(
              QPoint(0, currentNewsTab->newsToolBar_->height()-1))
            );
    }
  } else {
    filterNewsAll_->setChecked(true);
    setNewsFilter(filterNewsAll_);
  }
}

void RSSListing::slotTimerUpdateFeeds()
{
  if (autoUpdatefeeds_ && updateAllFeedsAct_->isEnabled()) {
    slotGetAllFeeds();
  }
}

void RSSListing::slotShowUpdateAppDlg()
{
  UpdateAppDialog *updateAppDialog = new UpdateAppDialog(langFileName_,
                                                         settings_, this);
  updateAppDialog->activateWindow();
  updateAppDialog->exec();
  delete updateAppDialog;
}

void RSSListing::appInstallTranslator()
{
  bool translatorLoad;
  qApp->removeTranslator(translator_);
#if defined(Q_OS_WIN) || defined(Q_OS_OS2)
  translatorLoad = translator_->load(QCoreApplication::applicationDirPath() +
                                     QString("/lang/quiterss_%1").arg(langFileName_.toLower()));
#else
  translatorLoad = translator_->load(QString("/usr/share/quiterss/lang/quiterss_%1").
                                     arg(langFileName_.toLower()));
#endif
  if (translatorLoad) qApp->installTranslator(translator_);
  else retranslateStrings();
}

void RSSListing::retranslateStrings() {
  feedsTitleLabel_->setText(tr("Feeds"));

  progressBar_->setFormat(tr("Update feeds... (%p%)"));

  QString str = statusUnread_->text();
  str = str.right(str.length() - str.indexOf(':') - 1).replace(" ", "");
  statusUnread_->setText(QString(tr(" Unread: %1 ")).arg(str));
  str = statusAll_->text();
  str = str.right(str.length() - str.indexOf(':') - 1).replace(" ", "");
  statusAll_->setText(QString(tr(" All: %1 ")).arg(str));

  str = traySystem->toolTip();
  QString info =
      "QuiteRSS\n" +
      QString(tr("New News: %1")).arg(str.section(": ", 1).section("\n", 0, 0)) +
      QString("\n") +
      QString(tr("Unread News: %1")).arg(str.section(": ", 2));
  traySystem->setToolTip(info);

  addFeedAct_->setText(tr("&Add Feed..."));
  addFeedAct_->setToolTip(tr("Add New Feed"));

  addFolderAct_->setText(tr("&Add Folder..."));
  addFolderAct_->setToolTip(tr("Add New Folder"));

  openFeedNewTabAct_->setText(tr("Open in New Tab"));

  deleteFeedAct_->setText(tr("&Delete..."));
  deleteFeedAct_->setToolTip(tr("Delete Selected Feed"));

  importFeedsAct_->setText(tr("&Import Feeds..."));
  importFeedsAct_->setToolTip(tr("Import Feeds from OPML File"));

  exportFeedsAct_->setText(tr("&Export Feeds..."));
  exportFeedsAct_->setToolTip(tr("Export Feeds to OPML File"));

  exitAct_->setText(tr("E&xit"));

  if (autoLoadImages_) {
    autoLoadImagesToggle_->setText(tr("Load Images"));
    autoLoadImagesToggle_->setToolTip(tr("Auto Load Images to News View"));
  } else {
    autoLoadImagesToggle_->setText(tr("No Load Images"));
    autoLoadImagesToggle_->setToolTip(tr("No Load Images to News View"));
  }

  updateFeedAct_->setText(tr("Update Feed"));
  updateFeedAct_->setToolTip(tr("Update Current Feed"));

  updateAllFeedsAct_->setText(tr("Update All"));
  updateAllFeedsAct_->setToolTip(tr("Update All Feeds"));

  markAllFeedsRead_->setText(tr("Mark All Feeds Read"));

  markNewsRead_->setText(tr("Mark Read/Unread"));
  markNewsRead_->setToolTip(tr("Mark Current News Read/Unread"));

  markAllNewsRead_->setText(tr("Mark All News Read"));
  markAllNewsRead_->setToolTip(tr("Mark All News Read"));


  setNewsFiltersAct_->setText(tr("News Filters..."));
  setFilterNewsAct_->setText(tr("Filter News..."));

  optionsAct_->setText(tr("Options..."));
  optionsAct_->setToolTip(tr("Open Options Dialog"));

  feedsFilter_->setText(tr("Filter"));
  filterFeedsAll_->setText(tr("Show All"));
  filterFeedsNew_->setText(tr("Show New"));
  filterFeedsUnread_->setText(tr("Show Unread"));
  filterFeedsStarred_->setText(tr("Show Starred Feeds"));

  newsFilter_->setText( tr("Filter"));
  filterNewsAll_->setText(tr("Show All"));
  filterNewsNew_->setText(tr("Show New"));
  filterNewsUnread_->setText(tr("Show Unread"));
  filterNewsStar_->setText(tr("Show Starred"));
  filterNewsNotStarred_->setText(tr("Show Not Starred"));
  filterNewsUnreadStar_->setText(tr("Show Unread or Starred"));
  filterNewsDeleted_->setText(tr("Show Deleted"));

  aboutAct_ ->setText(tr("About..."));
  aboutAct_->setToolTip(tr("Show 'About' Dialog"));

  updateAppAct_->setText(tr("Check for Updates..."));
  reportProblemAct_->setText(tr("Report a Problem..."));

  openInBrowserAct_->setText(tr("Open in Browser"));
  openInExternalBrowserAct_->setText(tr("Open in External Browser"));
  openInExternalBrowserAct_->setToolTip(tr("Open News in External Browser"));
  openNewsNewTabAct_->setText(tr("Open in New Tab"));
  openNewsNewTabAct_->setToolTip(tr("Open News in New Tab"));
  openNewsBackgroundTabAct_->setText(tr("Open in Background Tab"));
  openNewsBackgroundTabAct_->setToolTip(tr("Open News in Background Tab"));
  markStarAct_->setText(tr("Star"));
  markStarAct_->setToolTip(tr("Mark News Star"));
  deleteNewsAct_->setText(tr("Delete"));
  deleteNewsAct_->setToolTip(tr("Delete Selected News"));
  deleteAllNewsAct_->setText(tr("Delete All News"));
  deleteAllNewsAct_->setToolTip(tr("Delete All News from List"));
  restoreNewsAct_->setText(tr("Restore"));
  restoreNewsAct_->setToolTip(tr("Restore News"));

  markFeedRead_->setText(tr("Mark Read"));
  markFeedRead_->setToolTip(tr("Mark Feed Read"));
  feedProperties_->setText(tr("Feed Properties"));
  feedProperties_->setToolTip(tr("Feed Properties"));

  fileMenu_->setTitle(tr("&File"));
  editMenu_->setTitle(tr("&Edit"));
  viewMenu_->setTitle(tr("&View"));
  feedMenu_->setTitle(tr("Fee&ds"));
  newsMenu_->setTitle(tr("&News"));
  browserMenu_->setTitle(tr("&Browser"));
  toolsMenu_->setTitle(tr("&Tools"));
  helpMenu_->setTitle(tr("&Help"));

  mainToolbar_->setWindowTitle(tr("Main Toolbar"));
  customizeToolbarMenu_->setTitle(tr("Customize Toolbar"));
  toolBarStyleMenu_->setTitle(tr("Style"));
  toolBarStyleI_->setText(tr("Icon"));
  toolBarStyleT_->setText(tr("Text"));
  toolBarStyleTbI_->setText(tr("Text Beside Icon"));
  toolBarStyleTuI_->setText(tr("Text Under Icon"));
  toolBarToggle_->setText(tr("Hide Toolbar"));

  toolBarIconSizeMenu_->setTitle(tr("Icon Size"));
  toolBarIconBig_->setText(tr("Big"));
  toolBarIconNormal_->setText(tr("Normal"));
  toolBarIconSmall_->setText(tr("Small"));

  styleMenu_->setTitle(tr("Application Style"));
  systemStyle_->setText(tr("System"));
  system2Style_->setText(tr("System2"));
  greenStyle_->setText(tr("Green"));
  orangeStyle_->setText(tr("Orange"));
  purpleStyle_->setText(tr("Purple"));
  pinkStyle_->setText(tr("Pink"));
  grayStyle_->setText(tr("Gray"));

  browserPositionMenu_->setTitle(tr("Browser Position"));
  topBrowserPositionAct_->setText(tr("Top"));
  bottomBrowserPositionAct_->setText(tr("Bottom"));
  rightBrowserPositionAct_->setText(tr("Right"));
  leftBrowserPositionAct_->setText(tr("Left"));

  showWindowAct_->setText(tr("Show Window"));

  feedKeyUpAct_->setText(tr("Previous Feed"));
  feedKeyDownAct_->setText(tr("Next Feed"));
  newsKeyUpAct_->setText(tr("Previous News"));
  newsKeyDownAct_->setText(tr("Next News"));

  switchFocusAct_->setText(tr("Switch Focus Between Panels"));
  switchFocusAct_->setToolTip(
        tr("Switch Focus Between Panels (Tree Feeds, List News, Browser)"));

  visibleFeedsDockAct_->setText(tr("Show/Hide Tree Feeds"));

  placeToTrayAct_->setText(tr("Minimize to Tray"));
  placeToTrayAct_->setToolTip(
        tr("Minimize Application to Tray"));

  feedsColumnsMenu_->setTitle(tr("Columns"));
  showUnreadCount_->setText(tr("Count News Unread"));
  showUndeleteCount_->setText(tr("Count News All"));
  showLastUpdated_->setText(tr("Last Updated"));

  titleSortFeedsAct_->setText(tr("Sort by Title"));

  findFeedAct_->setToolTip(tr("Search Feed"));

  browserZoomMenu_->setTitle(tr("Zoom"));
  zoomInAct_->setText(tr("Zoom In"));
  zoomInAct_->setToolTip(tr("Zoom in in browser"));
  zoomOutAct_->setText(tr("Zoom Out"));
  zoomOutAct_->setToolTip(tr("Zoom out in browser"));
  zoomTo100Act_->setText(tr("100%"));
  zoomTo100Act_->setToolTip(tr("Reset zoom in browser"));

  printAct_->setText(tr("Print..."));
  printAct_->setToolTip(tr("Print Web page"));
  printPreviewAct_->setText(tr("Print Preview..."));
  printPreviewAct_->setToolTip(tr("Preview Web page"));

  toolbarsMenu_->setTitle(tr("Toolbars"));
  mainToolbarToggle_->setText(tr("Main Toolbar"));
  newsToolbarToggle_->setText(tr("News Toolbar"));
  browserToolbarToggle_->setText(tr("Browser Toolbar"));

  fullScreenAct_->setText(tr("Full Screen"));
  fullScreenAct_->setToolTip(tr("Full Screen"));

  QApplication::translate("QDialogButtonBox", "Cancel");
  QApplication::translate("QDialogButtonBox", "&Yes");
  QApplication::translate("QDialogButtonBox", "&No");

  QApplication::translate("QLineEdit", "&Undo");
  QApplication::translate("QLineEdit", "&Redo");
  QApplication::translate("QLineEdit", "Cu&t");
  QApplication::translate("QLineEdit", "&Copy");
  QApplication::translate("QLineEdit", "&Paste");
  QApplication::translate("QLineEdit", "Delete");
  QApplication::translate("QLineEdit", "Select All");

  QApplication::translate("QTextControl", "&Undo");
  QApplication::translate("QTextControl", "&Redo");
  QApplication::translate("QTextControl", "Cu&t");
  QApplication::translate("QTextControl", "&Copy");
  QApplication::translate("QTextControl", "&Paste");
  QApplication::translate("QTextControl", "Delete");
  QApplication::translate("QTextControl", "Select All");
  QApplication::translate("QTextControl", "Copy &Link Location");

  QApplication::translate("QAbstractSpinBox", "&Step up");
  QApplication::translate("QAbstractSpinBox", "Step &down");
  QApplication::translate("QAbstractSpinBox", "&Select All");

  QApplication::translate("QMultiInputContext", "Select IM");

  QApplication::translate("QWizard", "Cancel");
  QApplication::translate("QWizard", "< &Back");
  QApplication::translate("QWizard", "&Finish");
  QApplication::translate("QWizard", "&Next >");

  if (newsView_) {
    currentNewsTab->retranslateStrings();
  }
  findFeeds_->retranslateStrings();
}

void RSSListing::setToolBarStyle(QAction *pAct)
{
  if (pAct->objectName() == "toolBarStyleI_") {
    mainToolbar_->setToolButtonStyle(Qt::ToolButtonIconOnly);
  } else if (pAct->objectName() == "toolBarStyleT_") {
    mainToolbar_->setToolButtonStyle(Qt::ToolButtonTextOnly);
  } else if (pAct->objectName() == "toolBarStyleTbI_") {
    mainToolbar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  } else {
    mainToolbar_->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
  }
}

void RSSListing::setToolBarIconSize(QAction *pAct)
{
  if (pAct->objectName() == "toolBarIconBig_") {
    mainToolbar_->setIconSize(QSize(32, 32));
  } else if (pAct->objectName() == "toolBarIconSmall_") {
    mainToolbar_->setIconSize(QSize(16, 16));
  } else {
    mainToolbar_->setIconSize(QSize(24, 24));
  }
}

void RSSListing::showContextMenuToolBar(const QPoint &p)
{
  mainToolbarMenu_->popup(mainToolbar_->mapToGlobal(p));
}

void RSSListing::slotShowFeedPropertiesDlg()
{
  if (!feedsView_->selectIndex.isValid()){
    feedProperties_->setEnabled(false);
    return;
  }

  QModelIndex index = feedsView_->selectIndex;

  FeedPropertiesDialog *feedPropertiesDialog = new FeedPropertiesDialog(this);
  feedPropertiesDialog->restoreGeometry(settings_->value("feedProperties/geometry").toByteArray());

  QByteArray byteArray = feedsModel_->index(index.row(), feedsModel_->fieldIndex("image")).
      data().toByteArray();
  if (!byteArray.isNull()) {
    QPixmap icon;
    icon.loadFromData(QByteArray::fromBase64(byteArray));
    feedPropertiesDialog->setWindowIcon(icon);
  } else feedPropertiesDialog->setWindowIcon(QPixmap(":/images/feed"));
  QString str(feedPropertiesDialog->windowTitle() +
              " '" +
              feedsModel_->index(index.row(), feedsModel_->fieldIndex("text")).data().toString() +
              "'");
  feedPropertiesDialog->setWindowTitle(str);

  FEED_PROPERTIES properties;

  properties.general.text =
      feedsModel_->record(index.row()).field("text").value().toString();
  properties.general.title =
      feedsModel_->record(index.row()).field("title").value().toString();
  properties.general.url =
      feedsModel_->record(index.row()).field("xmlUrl").value().toString();
  properties.general.homepage =
      feedsModel_->record(index.row()).field("htmlUrl").value().toString();
  properties.general.displayOnStartup =
      feedsModel_->record(index.row()).field("displayOnStartup").value().toInt();
  properties.display.displayEmbeddedImages =
      feedsModel_->record(index.row()).field("displayEmbeddedImages").value().toInt();

  if (feedsModel_->record(index.row()).field("label").value().toString().contains("starred"))
    properties.general.starred = true;
  else
    properties.general.starred = false;

  QDateTime dtLocalTime = QDateTime::currentDateTime();
  QDateTime dtUTC = QDateTime(dtLocalTime.date(), dtLocalTime.time(), Qt::UTC);
  int nTimeShift = dtLocalTime.secsTo(dtUTC);

  QDateTime dt = QDateTime::fromString(
        feedsModel_->record(index.row()).field("created").value().toString(),
        Qt::ISODate);
  properties.status.createdTime = dt.addSecs(nTimeShift);

  dt = QDateTime::fromString(
        feedsModel_->record(index.row()).field("updated").value().toString(),
        Qt::ISODate);
  properties.status.lastUpdate = dt.addSecs(nTimeShift);

  properties.status.undeleteCount =
      feedsModel_->record(index.row()).field("undeleteCount").value().toInt();
  properties.status.newCount =
      feedsModel_->record(index.row()).field("newCount").value().toInt();
  properties.status.unreadCount =
      feedsModel_->record(index.row()).field("unread").value().toInt();
  properties.status.description =
      feedsModel_->record(index.row()).field("description").value().toString();

  feedPropertiesDialog->setFeedProperties(properties);

  connect(feedPropertiesDialog, SIGNAL(signalLoadTitle(QUrl, QUrl)),
          faviconLoader, SLOT(requestUrl(QUrl, QUrl)));
  connect(feedPropertiesDialog, SIGNAL(startGetUrlTimer()),
          this, SIGNAL(startGetUrlTimer()));

  int result = feedPropertiesDialog->exec();
  settings_->setValue("feedProperties/geometry", feedPropertiesDialog->saveGeometry());
  if (result == QDialog::Rejected) {
    delete feedPropertiesDialog;
    return;
  }

  int id = feedsModel_->record(index.row()).field("id").value().toInt();

  properties = feedPropertiesDialog->getFeedProperties();
  delete feedPropertiesDialog;

  QSqlQuery q(db_);
  q.prepare("update feeds set text = ?, xmlUrl = ?, displayOnStartup = ?, "
            "displayEmbeddedImages = ?, label = ? where id == ?");
  q.addBindValue(properties.general.text);
  q.addBindValue(properties.general.url);
  q.addBindValue(properties.general.displayOnStartup);
  q.addBindValue(properties.display.displayEmbeddedImages);
  if (properties.general.starred)
    q.addBindValue("starred");
  else
    q.addBindValue("");
  q.addBindValue(id);
  q.exec();

  QModelIndex currentIndex = feedsView_->currentIndex();
  feedsModel_->select();
  feedsView_->updateCurrentIndex(currentIndex);

  if (currentIndex == index) {
    QPixmap iconTab;
    byteArray = feedsModel_->index(
          index.row(), feedsModel_->fieldIndex("image")).data().toByteArray();
    if (!byteArray.isNull()) {
      iconTab.loadFromData(QByteArray::fromBase64(byteArray));
    } else {
      iconTab.load(":/images/feed");
    }
    currentNewsTab->newsIconTitle_->setPixmap(iconTab);

    QString tabText = feedsModel_->index(index.row(), feedsModel_->fieldIndex("text")).data().toString();
    currentNewsTab->newsTitleLabel_->setToolTip(tabText);
    tabText = currentNewsTab->newsTextTitle_->fontMetrics().elidedText(
          tabText, Qt::ElideRight, 114);
    currentNewsTab->newsTextTitle_->setText(tabText);
  }
}

void RSSListing::slotFeedMenuShow()
{
  if (feedsView_->selectIndex.isValid()) feedProperties_->setEnabled(true);
  else feedProperties_->setEnabled(false);
}

//! Обновление информации в трее: значок и текст подсказки
void RSSListing::refreshInfoTray()
{
  if (!showTrayIcon_) return;

  // Подсчёт количества новых и прочитанных новостей
  int newCount = 0;
  int unreadCount = 0;
  QSqlQuery q(db_);
  q.exec("SELECT newCount, unread FROM feeds");
  while (q.next()) {
    newCount    += q.value(0).toInt();
    unreadCount += q.value(1).toInt();
  }

  // Установка текста всплывающей подсказки
  QString info =
      "QuiteRSS\n" +
      QString(tr("New News: %1")).arg(newCount) +
      QString("\n") +
      QString(tr("Unread News: %1")).arg(unreadCount);
  traySystem->setToolTip(info);

  // Отображаем количество либо новых, либо непрочитанных новостей
  if (behaviorIconTray_ > CHANGE_ICON_TRAY) {
    int trayCount = (behaviorIconTray_ == UNREAD_COUNT_ICON_TRAY) ? unreadCount : newCount;
    // выводим иконку с цифрой
    if (trayCount != 0) {
      // Подготавливаем цифру
      QString trayCountStr;
      QFont font("Consolas");
      if (trayCount > 99) {
        font.setBold(false);
        if (trayCount < 1000) {
          font.setPixelSize(8);
          trayCountStr = QString::number(trayCount);
        } else {
          font.setPixelSize(11);
          trayCountStr = "#";
        }
      } else {
        font.setBold(true);
        font.setPixelSize(11);
        trayCountStr = QString::number(trayCount);
      }

      // Рисуем иконку, текст на ней, и устанавливаем разрисованную иконку в трей
      QPixmap icon = QPixmap(":/images/countNew");
      QPainter trayPainter;
      trayPainter.begin(&icon);
      trayPainter.setFont(font);
      trayPainter.setPen(Qt::white);
      trayPainter.drawText(QRect(1, 0, 15, 16), Qt::AlignVCenter | Qt::AlignHCenter,
                           trayCountStr);
      trayPainter.end();
      traySystem->setIcon(icon);
    }
    // Выводим иконку без цифры
    else {
#if defined(QT_NO_DEBUG_OUTPUT)
      traySystem->setIcon(QIcon(":/images/quiterss16"));
#else
      traySystem->setIcon(QIcon(":/images/quiterssDebug"));
#endif
    }
  }
}

//! Помечаем все ленты прочитанными
void RSSListing::markAllFeedsRead()
{
  db_.transaction();
  QSqlQuery q(db_);
  q.exec("UPDATE news SET new=0, read=2");
  q.exec("UPDATE feeds SET newCount=0, unread=0");
  db_.commit();

  if (tabWidget_->currentIndex() == 0) {
    feedsView_->setCurrentIndex(feedsModel_->index(-1, feedsModel_->fieldIndex("text")));
    slotFeedsTreeClicked(feedsModel_->index(-1, feedsModel_->fieldIndex("text")));
  } else {
    feedsModelReload();

    NewsTabWidget *widget = (NewsTabWidget*)tabWidget_->widget(0);

    int rowFeeds = -1;
    for (int i = 0; i < feedsModel_->rowCount(); i++) {
      if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() == widget->feedId_) {
        rowFeeds = i;
      }
    }

    if (rowFeeds == -1) {
      widget->newsModel_->setFilter("feedId=-1");

      QPixmap iconTab;
      iconTab.load(":/images/feed");
      widget->newsIconTitle_->setPixmap(iconTab);
      widget->newsTextTitle_->setText("");

      feedProperties_->setEnabled(false);
      widget->setVisible(false);
    }
  }

  refreshInfoTray();
}

//! Помечаем все ленты не новыми
void RSSListing::markAllFeedsOld()
{
  db_.transaction();
  QSqlQuery q(db_);
  q.exec("UPDATE news SET new=0");
  q.exec("UPDATE feeds SET newCount=0");
  db_.commit();

  feedsModelReload();

  if (currentNewsTab != NULL) {
    int currentRow = newsView_->currentIndex().row();

    setNewsFilter(newsFilterGroup_->checkedAction(), false);

    while (newsModel_->canFetchMore())
      newsModel_->fetchMore();

    newsView_->setCurrentIndex(newsModel_->index(currentRow, newsModel_->fieldIndex("title")));
  }
  refreshInfoTray();
}

void RSSListing::slotIconFeedLoad(const QString &strUrl, const QByteArray &byteArray)
{
  QSqlQuery q(db_);
  q.prepare("update feeds set image = ? where xmlUrl == ?");
  q.addBindValue(byteArray.toBase64());
  q.addBindValue(strUrl);
  q.exec();

  q.exec(QString("SELECT id FROM feeds WHERE xmlUrl LIKE '%1'").
         arg(strUrl));
  if (q.next()) {
    for (int i = 0; i < tabWidget_->count(); i++) {
      NewsTabWidget *widget = (NewsTabWidget*)tabWidget_->widget(i);
      if (widget->feedId_ == q.value(0).toInt()) {
        QPixmap iconTab;
        if (!byteArray.isNull()) {
          iconTab.loadFromData(byteArray);
        } else {
          iconTab.load(":/images/feed");
        }
        widget->newsIconTitle_->setPixmap(iconTab);
        break;
      }
    }
  }

  feedsModelReload();
}

void RSSListing::playSoundNewNews()
{
  if (!playSoundNewNews_ && soundNewNews_) {
#if defined(Q_OS_WIN) || defined(Q_OS_OS2)
    QSound::play(soundNotifyPath_);
#else
    QProcess::startDetached(QString("play %1").arg(soundNotifyPath_));
#endif
    playSoundNewNews_ = true;
  }
}

void RSSListing::showNewsFiltersDlg(bool newFilter)
{
  NewsFiltersDialog *newsFiltersDialog = new NewsFiltersDialog(this, settings_);
  if (newFilter) {
    newsFiltersDialog->filtersTree->setCurrentItem(
          newsFiltersDialog->filtersTree->topLevelItem(
            newsFiltersDialog->filtersTree->topLevelItemCount()-1));
  }

  newsFiltersDialog->exec();

  delete newsFiltersDialog;
}

void RSSListing::showFilterRulesDlg()
{
  if (!feedsView_->selectIndex.isValid()) return;

  int feedId = feedsModel_->record(
        feedsView_->selectIndex.row()).field("id").value().toInt();

  FilterRulesDialog *filterRulesDialog = new FilterRulesDialog(
        this, -1, feedId);

  int result = filterRulesDialog->exec();
  if (result == QDialog::Rejected) {
    delete filterRulesDialog;
    return;
  }

  delete filterRulesDialog;

  showNewsFiltersDlg(true);
}

void RSSListing::slotUpdateAppChacking()
{
  updateAppDialog_ = new UpdateAppDialog(langFileName_, settings_, this, false);
  connect(updateAppDialog_, SIGNAL(signalNewVersion(bool)),
          this, SLOT(slotNewVersion(bool)), Qt::QueuedConnection);
}

void RSSListing::slotNewVersion(bool newVersion)
{
  delete updateAppDialog_;

  if (newVersion) {
    traySystem->showMessage(tr("Check for updates"),
                            tr("A new version of QuiteRSS..."));
    connect(traySystem, SIGNAL(messageClicked()),
            this, SLOT(slotShowUpdateAppDlg()));
  }
}

/*! \brief Обработка клавиш Up/Down в дереве лент *****************************/
void RSSListing::slotFeedUpPressed()
{
  if (!feedsView_->currentIndex().isValid()) {
    feedsView_->setCurrentIndex(feedsModel_->index(0, feedsModel_->fieldIndex("text")));
    slotFeedsTreeClicked(feedsModel_->index(0, feedsModel_->fieldIndex("text")));
    return;
  }

  int row = feedsView_->currentIndex().row();
  if (row == 0) return;
  else row--;
  feedsView_->setCurrentIndex(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
  slotFeedsTreeClicked(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
}

void RSSListing::slotFeedDownPressed()
{
  if (!feedsView_->currentIndex().isValid()) {
    feedsView_->setCurrentIndex(feedsModel_->index(0, feedsModel_->fieldIndex("text")));
    slotFeedsTreeClicked(feedsModel_->index(0, feedsModel_->fieldIndex("text")));
    return;
  }

  int row = feedsView_->currentIndex().row();
  if ((row+1) == feedsModel_->rowCount()) return;
  else row++;
  feedsView_->setCurrentIndex(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
  slotFeedsTreeClicked(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
}

/*! \brief Обработка клавиш Home/End в дереве лент *****************************/
void RSSListing::slotFeedHomePressed()
{
  int row = 0;
  feedsView_->setCurrentIndex(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
  slotFeedsTreeClicked(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
}

void RSSListing::slotFeedEndPressed()
{
  int row = feedsModel_->rowCount()-1;
  feedsView_->setCurrentIndex(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
  slotFeedsTreeClicked(feedsModel_->index(row, feedsModel_->fieldIndex("text")));
}

//! Удаление новостей в ленте по критериям
void RSSListing::feedsCleanUp(QString feedId)
{
  int cntT = 0;
  int cntNews = 0;

  QSqlQuery q(db_);
  QString qStr;
  qStr = QString("SELECT undeleteCount FROM feeds WHERE id=='%1'").
      arg(feedId);
  q.exec(qStr);
  if (q.next()) cntNews = q.value(0).toInt();

  qStr = QString("SELECT deleted, received, id, read, starred, published "
      "FROM news WHERE feedId=='%1'")
      .arg(feedId);
  q.exec(qStr);
  while (q.next()) {
    int newsId = q.value(2).toInt();
    int read = q.value(3).toInt();
    int starred = q.value(4).toInt();

    if ((neverUnreadCleanUp_ && (read == 0)) ||
        (neverStarCleanUp_ && (starred != 0)) ||
        q.value(0).toInt() != 0)
      continue;

    if (newsCleanUpOn_ && (cntT < (cntNews - maxNewsCleanUp_))) {
        qStr = QString("UPDATE news SET deleted=1, read=2 WHERE feedId=='%1' AND id='%2'").
            arg(feedId).arg(newsId);
//        qCritical() << "*01"  << feedId << q.value(5).toString()
//                 << q.value(1).toString() << cntNews
//                 << (cntNews - maxNewsCleanUp_);
        QSqlQuery qt(db_);
        qt.exec(qStr);
        cntT++;
        continue;
    }

    QDateTime dateTime = QDateTime::fromString(
          q.value(1).toString(),
          Qt::ISODate);
    if (dayCleanUpOn_ &&
        (dateTime.daysTo(QDateTime::currentDateTime()) > maxDayCleanUp_)) {
        qStr = QString("UPDATE news SET deleted=1, read=2 WHERE feedId=='%1' AND id='%2'").
            arg(feedId).arg(newsId);
//        qCritical() << "*02"  << feedId << q.value(5).toString()
//                 << q.value(1).toString() << cntNews
//                 << (cntNews - maxNewsCleanUp_);
        QSqlQuery qt(db_);
        qt.exec(qStr);
        cntT++;
        continue;
    }

    if (readCleanUp_) {
      qStr = QString("UPDATE news SET deleted=1 WHERE feedId=='%1' AND read!=0 AND id='%2'").
          arg(feedId).arg(newsId);
      QSqlQuery qt(db_);
      qt.exec(qStr);
      cntT++;
    }
  }

  int undeleteCount = 0;
  qStr = QString("SELECT count(id) FROM news WHERE feedId=='%1' AND deleted==0").
      arg(feedId);
  q.exec(qStr);
  if (q.next()) undeleteCount = q.value(0).toInt();

  int unreadCount = 0;
  qStr = QString("SELECT count(read) FROM news WHERE feedId=='%1' AND read==0 AND deleted==0").
      arg(feedId);
  q.exec(qStr);
  if (q.next()) unreadCount = q.value(0).toInt();

  qStr = QString("UPDATE feeds SET unread='%1', undeleteCount='%3' "
      "WHERE id=='%4'").
      arg(unreadCount).arg(undeleteCount).arg(feedId);
  q.exec(qStr);
}

//! Установка стиля оформления приложения
void RSSListing::setStyleApp(QAction *pAct)
{
  QString fileString;
  if (pAct->objectName() == "systemStyle_") {
    fileString = ":/style/systemStyle";
  } else if (pAct->objectName() == "system2Style_") {
    fileString = ":/style/system2Style";
  } else if (pAct->objectName() == "orangeStyle_") {
    fileString = ":/style/orangeStyle";
  } else if (pAct->objectName() == "purpleStyle_") {
    fileString = ":/style/purpleStyle";
  } else if (pAct->objectName() == "pinkStyle_") {
    fileString = ":/style/pinkStyle";
  } else if (pAct->objectName() == "grayStyle_") {
    fileString = ":/style/grayStyle";
  } else {
    fileString = ":/style/greenStyle";
  }

  QFile file(fileString);
  file.open(QFile::ReadOnly);
  qApp->setStyleSheet(QLatin1String(file.readAll()));
  file.close();
}

//! Переключение фокуса между деревом лент, списком новостей и браузером
void RSSListing::slotSwitchFocus()
{
  if (feedsView_->hasFocus()) {
    newsView_->setFocus();
  } else if (newsView_->hasFocus()) {
    webView_->setFocus();
  } else {
    feedsView_->setFocus();
  }
}

//! Открытие ленты в новой вкладке
void RSSListing::slotOpenFeedNewTab()
{
  feedsView_->setCurrentIndex(feedsView_->selectIndex);
  slotFeedsTreeSelected(feedsView_->selectIndex, true, true);
}

//! Закрытие вкладки
void RSSListing::slotTabCloseRequested(int index)
{
  if (index != 0) {
    NewsTabWidget *widget = (NewsTabWidget*)tabWidget_->widget(index);

    if (widget->feedId_ > -1) {
      setFeedRead(widget->feedId_, 1);

      QString stateStr;
      if(isMinimized()) {
        if (oldState & Qt::WindowMaximized)
          stateStr = "Maximized";
      } else {
        if (windowState() & Qt::WindowMaximized)
          stateStr = "Maximized";
      }
      QStringList widthStrList;
      for (int i = 0; i < widget->newsHeader_->count(); i++) {
        widthStrList << QString::number(widget->newsHeader_->sectionSize(i));
      }
      settings_->setValue("NewsHeaderSectionSize" + stateStr, widthStrList);
      settings_->setValue("NewsHeaderState", widget->newsHeader_->saveState());

      settings_->setValue("NewsTabSplitter",
                          widget->newsTabWidgetSplitter_->saveGeometry());
      settings_->setValue("NewsTabSplitter",
                          widget->newsTabWidgetSplitter_->saveState());
    }

    QWidget *newsTitleLabel = widget->newsTitleLabel_;
    delete widget;
    delete newsTitleLabel;
  }
}

//! Переключение между вкладками
void RSSListing::slotTabCurrentChanged(int index)
{
  if (tabCurrentUpdateOff_) return;

  if (tabWidget_->count()) {
    NewsTabWidget *widget = (NewsTabWidget*)tabWidget_->widget(index);
    if (widget->feedId_ > -1) {
      if (widget->feedId_ == 0)
        widget->hide();
      createNewsTab(index);

      int rowFeeds = -1;
      for (int i = 0; i < feedsModel_->rowCount(); i++) {
        if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data().toInt() == currentNewsTab->feedId_) {
          rowFeeds = i;
        }
      }
      feedsView_->setCurrentIndex(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));
      setFeedsFilter(feedsFilterGroup_->checkedAction(), false);

      slotUpdateNews();
      currentNewsTab->slotNewsViewSelected(newsView_->currentIndex());
      currentNewsTab->newsView_->setFocus();

      statusUnread_->setVisible(true);
      statusAll_->setVisible(true);
    } else {
      statusUnread_->setVisible(false);
      statusAll_->setVisible(false);
      widget->setSettings();
      widget->retranslateStrings();
      widget->setFocus();
    }
  }
}

//! Включение/отключение отображения колонок в дереве лент
void RSSListing::feedsColumnVisible(QAction *action)
{
  int idx = action->data().toInt();
  feedsView_->setColumnHidden(idx, !action->isChecked());
  if (action->isChecked())
    feedsTreeView_->showColumn(feedsTreeModel_->proxyColumnByOriginal(idx));
  else
    feedsTreeView_->hideColumn(feedsTreeModel_->proxyColumnByOriginal(idx));
}

//! Установка позиции браузера
void RSSListing::setBrowserPosition(QAction *action)
{
  browserPosition_ = action->data().toInt();
  currentNewsTab->setBrowserPosition();
}

//! Создание вкладки только с браузером
QWebPage *RSSListing::createWebTab()
{
  NewsTabWidget *widget = new NewsTabWidget(-1, this);
  int indexTab = tabWidget_->addTab(widget, "");

  tabBar_->setTabButton(indexTab,
                        QTabBar::LeftSide,
                        widget->newsTitleLabel_);

  widget->newsTextTitle_->setText(tr("Loading..."));

  QPixmap iconTab;
  iconTab.load(":/images/webPage");
  widget->newsIconTitle_->setPixmap(iconTab);

  widget->autoLoadImages_ = currentNewsTab->autoLoadImages_;
  widget->setSettings();
  widget->retranslateStrings();

  if (QApplication::keyboardModifiers() != Qt::ControlModifier)
    emit signalCurrentTab(indexTab);

  return widget->webView_->page();
}

void RSSListing::creatFeedTab(int feedId)
{
  QSqlQuery q(db_);
  q.exec(QString("SELECT text, image, currentNews FROM feeds WHERE id LIKE '%1'").
         arg(feedId));

  if (q.next()) {
    NewsTabWidget *widget = new NewsTabWidget(feedId, this);
    int indexTab = tabWidget_->addTab(widget, "");
    widget->setSettings();
    widget->retranslateStrings();
    widget->setBrowserPosition();
    tabBar_->setTabButton(indexTab,
                          QTabBar::LeftSide,
                          widget->newsTitleLabel_);

    //! Устанавливаем иконку и текст для открытой вкладки
    QPixmap iconTab;
    QByteArray byteArray = q.value(1).toByteArray();
    if (!byteArray.isNull()) {
      iconTab.loadFromData(QByteArray::fromBase64(byteArray));
    } else {
      iconTab.load(":/images/feed");
    }
    widget->newsIconTitle_->setPixmap(iconTab);

    QString tabText = q.value(0).toString();
    widget->newsTitleLabel_->setToolTip(tabText);
    tabText = currentNewsTab->newsTextTitle_->fontMetrics().elidedText(
          tabText, Qt::ElideRight, 114);
    widget->newsTextTitle_->setText(tabText);

    QString feedIdFilter(QString("feedId=%1 AND ").arg(feedId));
    if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsAll_") {
      feedIdFilter.append("deleted = 0");
    } else if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsNew_") {
      feedIdFilter.append(QString("new = 1 AND deleted = 0"));
    } else if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsUnread_") {
      feedIdFilter.append(QString("read < 2 AND deleted = 0"));
    } else if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsStar_") {
      feedIdFilter.append(QString("starred = 1 AND deleted = 0"));
    } else if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsNotStarred_") {
      feedIdFilter.append(QString("starred = 0 AND deleted = 0"));
    } else if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsUnreadStar_") {
      feedIdFilter.append(QString("(read < 2 OR starred = 1) AND deleted = 0"));
    } else if (newsFilterGroup_->checkedAction()->objectName() == "filterNewsDeleted_") {
      feedIdFilter.append(QString("deleted = 1"));
    }
    widget->newsModel_->setFilter(feedIdFilter);

    if (widget->newsModel_->rowCount() != 0) {
      while (widget->newsModel_->canFetchMore())
        widget->newsModel_->fetchMore();
    }

    // выбор новости ленты, отображамой ранее
    int newsRow = -1;
    if (openingFeedAction_ == 0) {
      for (int i = 0; i < widget->newsModel_->rowCount(); i++) {
        if (widget->newsModel_->index(i, widget->newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt() ==
            q.value(2).toInt()) {
          newsRow = i;
          break;
        }
      }
    } else if (openingFeedAction_ == 1) newsRow = 0;

    widget->newsView_->setCurrentIndex(widget->newsModel_->index(newsRow, widget->newsModel_->fieldIndex("title")));
    if (newsRow == -1)
      widget->newsView_->verticalScrollBar()->setValue(newsRow);

    if ((openingFeedAction_ < 2) && openNewsWebViewOn_) {
      widget->slotNewsViewSelected(widget->newsModel_->index(newsRow, widget->newsModel_->fieldIndex("title")));
    } else {
      widget->slotNewsViewSelected(widget->newsModel_->index(-1, widget->newsModel_->fieldIndex("title")));
      QSqlQuery q(db_);
      int newsId = widget->newsModel_->index(newsRow, widget->newsModel_->fieldIndex("id")).data(Qt::EditRole).toInt();
      QString qStr = QString("UPDATE feeds SET currentNews='%1' WHERE id=='%2'").arg(newsId).arg(feedId);
      q.exec(qStr);
    }
  }
}

//! Применение пользовательских фильтров
void RSSListing::setUserFilter(int feedId, int filterId)
{
  QSqlQuery q(db_);
  db_.transaction();
  bool onlyNew = true;

  if (filterId != -1) {
    onlyNew = false;
    q.exec(QString("SELECT enable, type FROM filters WHERE id='%1' AND feeds LIKE '\%,%2,\%'").
           arg(filterId).arg(feedId));
  } else {
    q.exec(QString("SELECT enable, type, id FROM filters WHERE feeds LIKE '\%,%1,\%' ORDER BY num").
           arg(feedId));
  }

  while (q.next()) {
    if (q.value(0).toInt() == 0) continue;

    if (onlyNew)
      filterId = q.value(2).toInt();
    int filterType = q.value(1).toInt();

    QString qStr("UPDATE news SET");
    QString qStr1;

    QSqlQuery q1(db_);
    q1.exec(QString("SELECT action FROM filterActions "
                    "WHERE idFilter=='%1'").arg(filterId));
    while (q1.next()) {
      if (!qStr1.isNull()) qStr1.append(",");
      switch (q1.value(0).toInt()) {
      case 0: // action -> Mark news as read
        qStr1.append(" read=2");
        break;
      case 1: // action -> Add star
        qStr1.append(" starred=1");
        break;
      case 2: // action -> Delete
        qStr1.append(" read=2, deleted=1");
        break;
      }
    }
    qStr.append(qStr1);
    qStr.append(QString(" WHERE feedId='%1' AND deleted=0").arg(feedId));

    if (onlyNew) qStr.append(" AND new=1");

    QString qStr2;
    switch (filterType) {
    case 1: // Match all conditions
      qStr2.append("AND ");
      break;
    case 2: // Match any condition
      qStr2.append("OR ");
      break;
    }

    if ((filterType == 1) || (filterType == 2)) {
      qStr.append(" AND ( ");
      qStr1.clear();

      q1.exec(QString("SELECT field, condition, content FROM filterConditions "
                      "WHERE idFilter=='%1'").arg(filterId));
      while (q1.next()) {
        if (!qStr1.isNull()) qStr1.append(qStr2);
        switch (q1.value(0).toInt()) {
        case 0: // field -> Title
          switch (q1.value(1).toInt()) {
          case 0: // condition -> contains
            qStr1.append(QString("title LIKE '\%%1\%' ").arg(q1.value(2).toString()));
            break;
          case 1: // condition -> doesn't contains
            qStr1.append(QString("title NOT LIKE '\%%1\%' ").arg(q1.value(2).toString()));
            break;
          case 2: // condition -> is
            qStr1.append(QString("title LIKE '%1' ").arg(q1.value(2).toString()));
            break;
          case 3: // condition -> isn't
            qStr1.append(QString("title NOT LIKE '%1' ").arg(q1.value(2).toString()));
            break;
          case 4: // condition -> begins with
            qStr1.append(QString("title LIKE '%1\%' ").arg(q1.value(2).toString()));
            break;
          case 5: // condition -> ends with
            qStr1.append(QString("title LIKE '\%%1' ").arg(q1.value(2).toString()));
            break;
          }
          break;
        case 1: // field -> Description
          switch (q1.value(1).toInt()) {
          case 0: // condition -> contains
            qStr1.append(QString("description LIKE '\%%1\%' ").arg(q1.value(2).toString()));
            break;
          case 1: // condition -> doesn't contains
            qStr1.append(QString("description NOT LIKE '\%%1\%' ").arg(q1.value(2).toString()));
            break;
          }
          break;
        case 2: // field -> Author
          switch (q1.value(1).toInt()) {
          case 0: // condition -> contains
            qStr1.append(QString("author_name LIKE '\%%1\%' ").arg(q1.value(2).toString()));
            break;
          case 1: // condition -> doesn't contains
            qStr1.append(QString("author_name NOT LIKE '\%%1\%' ").arg(q1.value(2).toString()));
            break;
          case 2: // condition -> is
            qStr1.append(QString("author_name LIKE '%1' ").arg(q1.value(2).toString()));
            break;
          case 3: // condition -> isn't
            qStr1.append(QString("author_name NOT LIKE '%1' ").arg(q1.value(2).toString()));
            break;
          }
          break;
        case 3: // field -> Category
          switch (q1.value(1).toInt()) {
          case 0: // condition -> is
            qStr1.append(QString("category LIKE '%1' ").arg(q1.value(2).toString()));
            break;
          case 1: // condition -> isn't
            qStr1.append(QString("category NOT LIKE '%1' ").arg(q1.value(2).toString()));
            break;
          case 2: // condition -> begins with
            qStr1.append(QString("category LIKE '%1\%' ").arg(q1.value(2).toString()));
            break;
          case 3: // condition -> ends with
            qStr1.append(QString("category LIKE '\%%1' ").arg(q1.value(2).toString()));
            break;
          }
          break;
        case 4: // field -> Status
          if (q1.value(1).toInt() == 0) { // Status -> is
            switch (q1.value(2).toInt()) {
            case 0:
              qStr1.append("new==1 ");
              break;
            case 1:
              qStr1.append("read>=1 ");
              break;
            case 2:
              qStr1.append("starred==1 ");
              break;
            }
          } else { // Status -> isn't
            switch (q1.value(2).toInt()) {
            case 0:
              qStr1.append("new==0 ");
              break;
            case 1:
              qStr1.append("read==0 ");
              break;
            case 2:
              qStr1.append("starred==0 ");
              break;
            }
          }
          break;
        }
      }
      qStr.append(qStr1).append(")");
      q1.exec(qStr);
//      qCritical() << qStr;
    }
  }
  q.exec(QString("UPDATE news SET new=0 WHERE feedId='%1' AND read=2 AND new=1")
         .arg(feedId));
  db_.commit();
}

//! Открытие новости клавишей Enter
void RSSListing::slotOpenNewsWebView()
{
  if (!newsView_->hasFocus()) return;
  currentNewsTab->slotNewsViewClicked(newsView_->currentIndex());
}

void RSSListing::slotNewsUpPressed()
{
  currentNewsTab->slotNewsUpPressed();
}

void RSSListing::slotNewsDownPressed()
{
  currentNewsTab->slotNewsDownPressed();
}

void RSSListing::markNewsRead()
{
  currentNewsTab->markNewsRead();
}

void RSSListing::markAllNewsRead()
{
  currentNewsTab->markAllNewsRead();
}

void RSSListing::markNewsStar()
{
  currentNewsTab->markNewsStar();
}

void RSSListing::deleteNews()
{
  currentNewsTab->deleteNews();
}

void RSSListing::deleteAllNewsList()
{
  currentNewsTab->deleteAllNewsList();
}

void RSSListing::restoreNews()
{
  currentNewsTab->restoreNews();
}

void RSSListing::openInBrowserNews()
{
  currentNewsTab->openInBrowserNews();
}

void RSSListing::openInExternalBrowserNews()
{
  currentNewsTab->openInExternalBrowserNews();
}

void RSSListing::slotOpenNewsNewTab()
{
  currentNewsTab->openNewsNewTab();
}

void RSSListing::slotOpenNewsBackgroundTab()
{
  currentNewsTab->openNewsNewTab();
  QKeyEvent* pe = new QKeyEvent(QEvent::KeyPress,
                                Qt::ControlModifier,
                                Qt::NoModifier);
  QApplication::sendEvent(this, pe);
}

//! Перечитывание модели лент
void RSSListing::feedsModelReload()
{
  int rowFeeds = feedsView_->currentIndex().row();
  int id = feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("id")).data().toInt();
  feedsModel_->select();
  rowFeeds = -1;
  for (int i = 0; i < feedsModel_->rowCount(); i++) {
    if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data().toInt() == id) {
      rowFeeds = i;
    }
  }
  feedsView_->updateCurrentIndex(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));
}

void RSSListing::setCurrentTab(int index, bool updateTab)
{
  tabCurrentUpdateOff_ = updateTab;
  tabWidget_->setCurrentIndex(index);
  tabCurrentUpdateOff_ = false;
}

//! Установить фокус в строку поиска (CTRL+F)
void RSSListing::findText()
{
  if (currentNewsTab->feedId_ > -1)
    currentNewsTab->findText_->setFocus();
}

//! Показать уведовление о входящих новостях
void RSSListing::showNotification()
{
  if (idFeedList_.isEmpty() || isActiveWindow() || !showNotifyOn_) return;

  if (notificationWidget) delete notificationWidget;
  notificationWidget = new NotificationWidget(
        &db_, idFeedList_, cntNewNewsList_,
        countShowNewsNotify_, timeShowNewsNotify_, widthTitleNewsNotify_,
        notificationFontFamily_, notificationFontSize_);

  connect(notificationWidget, SIGNAL(signalShow()), this, SLOT(slotShowWindows()));
  connect(notificationWidget, SIGNAL(signalDelete()),
          this, SLOT(deleteNotification()));
  connect(notificationWidget, SIGNAL(signalOpenNews(int,int)),
          this, SLOT(slotOpenNew(int,int)));

  notificationWidget->show();
}

//! Удалить уведовление о входящих новостях
void RSSListing::deleteNotification()
{
  notificationWidget->deleteLater();
  notificationWidget = NULL;
}

//! Показать новость при клике в окне уведомления входящих новостей
void RSSListing::slotOpenNew(int feedId, int newsId)
{
  deleteNotification();

  QSqlQuery q(db_);
  QString qStr = QString("UPDATE feeds SET currentNews='%1' WHERE id=='%2'").arg(newsId).arg(feedId);
  q.exec(qStr);

  int rowFeeds = -1;
  for (int i = 0; i < feedsModel_->rowCount(); i++) {
    if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data().toInt() == feedId) {
      rowFeeds = i;
    }
  }
  openingFeedAction_ = 0;
  openNewsWebViewOn_ = true;
  feedIdOld = -2;

  feedsView_->setCurrentIndex(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));
  slotFeedsTreeClicked(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));

  openingFeedAction_ = settings_->value("/Settings/openingFeedAction", 0).toInt();
  openNewsWebViewOn_ = settings_->value("/Settings/openNewsWebViewOn", true).toBool();
  QModelIndex index = newsView_->currentIndex();
  slotShowWindows();
  newsView_->setCurrentIndex(index);
}

void RSSListing::slotFindFeeds(QString text)
{
  if (!findFeedsWidget_->isVisible()) return;

  QString filterStr;
  if (feedsFilterGroup_->checkedAction()->objectName() == "filterFeedsAll_") {
    filterStr = "";
  } else if (feedsFilterGroup_->checkedAction()->objectName() == "filterFeedsNew_") {
    filterStr = QString("newCount > 0 AND ");
  } else if (feedsFilterGroup_->checkedAction()->objectName() == "filterFeedsUnread_") {
    filterStr = QString("unread > 0 AND ");
  } else if (feedsFilterGroup_->checkedAction()->objectName() == "filterFeedsStarred_") {
    filterStr = QString("label LIKE '\%starred\%' AND ");
  }

  if (findFeeds_->findGroup_->checkedAction()->objectName() == "findNameAct") {
    filterStr.append(QString("text LIKE '\%%1\%'").arg(text));
  } else {
    filterStr.append(QString("xmlUrl LIKE '\%%1\%'").arg(text));
  }
  feedsModel_->setFilter(filterStr);
}

void RSSListing::slotSelectFind()
{
  slotFindFeeds(findFeeds_->text());
}

void RSSListing::findFeedVisible(bool visible)
{
  findFeedsWidget_->setVisible(visible);
  if (visible) {
    findFeeds_->setFocus();
  } else {
    findFeeds_->clear();
    feedsModel_->setFilter("");
    int rowFeeds = -1;
    for (int i = 0; i < feedsModel_->rowCount(); i++) {
      if (feedsModel_->index(i, feedsModel_->fieldIndex("id")).data().toInt() == currentNewsTab->feedId_) {
        rowFeeds = i;
      }
    }
    feedsView_->setCurrentIndex(feedsModel_->index(rowFeeds, feedsModel_->fieldIndex("text")));
    setFeedsFilter(feedsFilterGroup_->checkedAction(), false);
  }
}

//! Полное удаление новостей
void RSSListing::cleanUp()
{
  QSqlQuery q(db_);

  db_.transaction();
  bool lastBuildDateClear = false;
  q.exec("SELECT value FROM info WHERE name='lastBuildDateClear_0.10.1'");
  if (q.next()) {
    lastBuildDateClear = q.value(0).toBool();
    q.exec("UPDATE info SET value='true' WHERE name='lastBuildDateClear_0.10.1'");
  }
  else q.exec("INSERT INTO info(name, value) VALUES ('lastBuildDateClear_0.10.1', 'true')");

  if (!lastBuildDateClear) {
    QString qStr = QString("UPDATE feeds SET lastBuildDate = '%1'").
        arg(QDateTime().toString(Qt::ISODate));
    q.exec(qStr);
  }

  if (settings_->value("CleanUp", 0).toInt() != 1) return;

  q.exec("SELECT received, id FROM news WHERE deleted==2");
  while (q.next()) {
    QDateTime dateTime = QDateTime::fromString(q.value(0).toString(), Qt::ISODate);
    if (dateTime.daysTo(QDateTime::currentDateTime()) > settings_->value("DayCleanUp", 0).toInt()) {
      QString qStr = QString("DELETE FROM news WHERE id='%2'").
          arg(q.value(1).toInt());
      QSqlQuery qt(db_);
      qt.exec(qStr);
    }
  }
  db_.commit();

  settings_->setValue("CleanUp", 0);
}

//! Сортировка дерева лент
void RSSListing::slotSortFeeds()
{
  if (titleSortFeedsAct_->isChecked())
    feedsView_->sortByColumn(feedsModel_->fieldIndex("text"), Qt::AscendingOrder);
  else
    feedsView_->sortByColumn(feedsModel_->fieldIndex("id"), Qt::AscendingOrder);
}

//! Масштаб в браузере
void RSSListing::browserZoom(QAction *action)
{
  if (action->objectName() == "zoomInAct") {
    webView_->setZoomFactor(webView_->zoomFactor()+0.1);
  } else if (action->objectName() == "zoomOutAct") {
    if (webView_->zoomFactor() > 0.1)
      webView_->setZoomFactor(webView_->zoomFactor()-0.1);
  } else {
    webView_->setZoomFactor(1);
  }
}

//! Сообщить о проблеме...
void RSSListing::slotReportProblem()
{
  QDesktopServices::openUrl(QUrl("http://code.google.com/p/quite-rss/issues/list"));
}

//! Печать страницы из браузера
void RSSListing::slotPrint()
{
  QPrinter printer;
  printer.setDocName(tr("Web Page"));
  QPrintDialog *printDlg = new QPrintDialog(&printer);
  connect(printDlg, SIGNAL(accepted(QPrinter*)), webView_, SLOT(print(QPrinter*)));
  printDlg->exec();
  delete printDlg;
}

//! Предварительный просмотр при печати страницы из браузера
void RSSListing::slotPrintPreview()
{
  QPrinter printer;
  printer.setDocName(tr("Web Page"));
  QPrintPreviewDialog *prevDlg = new QPrintPreviewDialog(&printer);
  prevDlg->setWindowFlags(prevDlg->windowFlags() | Qt::WindowMaximizeButtonHint);
  prevDlg->resize(650, 800);
  connect(prevDlg, SIGNAL(paintRequested(QPrinter*)), webView_, SLOT(print(QPrinter*)));
  prevDlg->exec();
  delete prevDlg;
}

void RSSListing::setFullScreen()
{
  if (!isFullScreen()) {
    // hide menu & toolbars
    mainToolbar_->hide();
    menuBar()->hide();
#ifdef Q_WS_X11
    show();
    raise();
    setWindowState( windowState() | Qt::WindowFullScreen );
#else
    setWindowState( windowState() | Qt::WindowFullScreen );
    show();
    raise();
#endif
  } else {
    menuBar()->show();
    mainToolbar_->show();
    setWindowState( windowState() ^ Qt::WindowFullScreen );
    show();
  }
}
