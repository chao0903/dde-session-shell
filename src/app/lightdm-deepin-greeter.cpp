// SPDX-FileCopyrightText: 2015 - 2022 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "accessibilitycheckerex.h"
#include "appeventfilter.h"
#include "constants.h"
#include "greeterworker.h"
#include "loginwindow.h"
#include "modules_loader.h"
#include "multiscreenmanager.h"
#include "propertygroup.h"
#include "sessionbasemodel.h"

#include <DApplication>
#include <DGuiApplicationHelper>
#include <DLog>
#include <DPlatformTheme>

#include <QGuiApplication>
#include <QScreen>
#include <QSettings>

#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib-xcb.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <cstdlib>
#include <DConfig>
#include <cmath>

DCORE_USE_NAMESPACE
DWIDGET_USE_NAMESPACE

const bool IsWayland = qgetenv("XDG_SESSION_TYPE").contains("wayland");

//Load the System cursor --begin
static XcursorImages*
xcLoadImages(const char *image, int size)
{
    QSettings settings(DDESESSIONCC::DEFAULT_CURSOR_THEME, QSettings::IniFormat);
    //The default cursor theme path
    qDebug() << "Theme Path:" << DDESESSIONCC::DEFAULT_CURSOR_THEME;
    QString item = "Icon Theme";
    const char* defaultTheme = "";
    QVariant tmpCursorTheme = settings.value(item + "/Inherits");
    if (tmpCursorTheme.isNull()) {
        qDebug() << "Set a default one instead, if get the cursor theme failed!";
        defaultTheme = "Deepin";
    } else {
        defaultTheme = tmpCursorTheme.toString().toLocal8Bit().data();
    }

    qDebug() << "Get defaultTheme:" << tmpCursorTheme.isNull()
             << defaultTheme;
    return XcursorLibraryLoadImages(image, defaultTheme, size);
}

static unsigned long loadCursorHandle(Display *dpy, const char *name, int size)
{
    if (size == -1) {
        size = XcursorGetDefaultSize(dpy);
    }

    // Load the cursor images
    XcursorImages *images = nullptr;
    images = xcLoadImages(name, size);

    if (!images) {
        images = xcLoadImages(name, size);
        if (!images) {
            return 0;
        }
    }

    unsigned long handle = static_cast<unsigned long>(XcursorImagesLoadCursor(dpy,images));
    XcursorImagesDestroy(images);

    return handle;
}

static int set_rootwindow_cursor() {
    Display* display = XOpenDisplay(nullptr);
    if (!display) {
        qDebug() << "Open display failed";
        return -1;
    }

    const char *cursorPath = qApp->devicePixelRatio() > 1.7
            ? "/usr/share/icons/bloom/cursors/loginspinner@2x"
            : "/usr/share/icons/bloom/cursors/loginspinner";

    Cursor cursor = static_cast<Cursor>(XcursorFilenameLoadCursor(display, cursorPath));
    if (cursor == 0) {
        cursor = static_cast<Cursor>(loadCursorHandle(display, "watch", 24));
    }
    XDefineCursor(display, XDefaultRootWindow(display),cursor);

    // XFixesChangeCursorByName is the key to change the cursor
    // and the XFreeCursor and XCloseDisplay is also essential.

    XFixesChangeCursorByName(display, cursor, "watch");

    XFreeCursor(display, cursor);
    XCloseDisplay(display);

    return 0;
}
// Load system cursor --end

bool isScaleConfigExists() {
    QDBusInterface configInter("com.deepin.system.Display",
                                                     "/com/deepin/system/Display",
                                                     "com.deepin.system.Display",
                                                    QDBusConnection::systemBus());
    if (!configInter.isValid()) {
        return false;
    }
    QDBusReply<QString> configReply = configInter.call("GetConfig");
    if (configReply.isValid()) {
        QString config = configReply.value();
        QJsonParseError jsonError;
        QJsonDocument jsonDoc(QJsonDocument::fromJson(config.toStdString().data(), &jsonError));
        if (jsonError.error == QJsonParseError::NoError) {
            QJsonObject rootObj = jsonDoc.object();
            QJsonObject Config = rootObj.value("Config").toObject();
            return !Config.value("ScaleFactors").toObject().isEmpty();
        } else {
            return false;
        }
    } else {
        qWarning() << configReply.error().message();
        return false;
    }
}

// 参照后端算法，保持一致
float toListedScaleFactor(float s)  {
	const float min = 1.0, max = 3.0, step = 0.25;
	if (s <= min) {
		return min;
	} else if (s >= max) {
		return max;
	}

	for (float i = min; i <= max; i += step) {
		if (i > s) {
			float ii = i - step;
			float d1 = s - ii;
			float d2 = i - s;
			if (d1 >= d2) {
				return i;
			} else {
				return ii;
			}
		}
	}
	return max;
}

static double get_scale_ratio() {
    Display *display = XOpenDisplay(nullptr);
    double scaleRatio = 0.0;

    if (!display) {
        return scaleRatio;
    }

    XRRScreenResources *resources = XRRGetScreenResourcesCurrent(display, DefaultRootWindow(display));

    if (!resources) {
        resources = XRRGetScreenResources(display, DefaultRootWindow(display));
        qWarning() << "get XRRGetScreenResourcesCurrent failed, use XRRGetScreenResources.";
    }

    if (resources) {
        for (int i = 0; i < resources->noutput; i++) {
            XRROutputInfo* outputInfo = XRRGetOutputInfo(display, resources, resources->outputs[i]);
            if (outputInfo->crtc == 0 || outputInfo->mm_width == 0) continue;

            XRRCrtcInfo *crtInfo = XRRGetCrtcInfo(display, resources, outputInfo->crtc);
            if (crtInfo == nullptr) continue;
            // 参照后端的算法，与后端保持一致
            float lenPx = hypot(static_cast<double>(crtInfo->width), static_cast<double>(crtInfo->height));
            float lenMm = hypot(static_cast<double>(outputInfo->mm_width), static_cast<double>(outputInfo->mm_height));
            float lenPxStd = hypot(1920, 1080);
            float lenMmStd = hypot(477, 268);
            float fix = (lenMm - lenMmStd) * (lenPx / lenPxStd) * 0.00158;
            float scale = (lenPx / lenMm) / (lenPxStd / lenMmStd) + fix;
            scaleRatio = toListedScaleFactor(scale);
        }
    }
    else {
        qWarning() << "get scale radio failed, please check X11 Extension.";
    }

    return scaleRatio;
}

// 读取系统配置文件的缩放比，如果是null，默认返回1
double getScaleFormConfig()
{
    double defaultScaleFactors = 1.0;
    DTK_CORE_NAMESPACE::DConfig * dconfig = DConfig::create("org.deepin.dde.lightdm-deepin-greeter", "org.deepin.dde.lightdm-deepin-greeter", QString(), nullptr);
    //华为机型,从override配置中获取默认缩放比
    if (dconfig) {
        defaultScaleFactors = dconfig->value("defaultScaleFactors", 1.0).toDouble() ;
        qDebug() << Q_FUNC_INFO <<"defaultScaleFactors :" << defaultScaleFactors;
        if(defaultScaleFactors < 1.0){
            defaultScaleFactors = 1.0;
        }
    }

    delete dconfig;
    dconfig = nullptr;

    QDBusInterface configInter("com.deepin.system.Display",
                                                     "/com/deepin/system/Display",
                                                     "com.deepin.system.Display",
                                                    QDBusConnection::systemBus());
    if (!configInter.isValid()) {
        return defaultScaleFactors;
    }
    QDBusReply<QString> configReply = configInter.call("GetConfig");
    if (configReply.isValid()) {
        QString config = configReply.value();
        QJsonParseError jsonError;
        QJsonDocument jsonDoc(QJsonDocument::fromJson(config.toLatin1(), &jsonError));
        if (jsonError.error == QJsonParseError::NoError) {
            QJsonObject rootObj = jsonDoc.object();
            QJsonObject Config = rootObj.value("Config").toObject();
            double scaleFactors = Config.value("ScaleFactors").toObject().value("ALL").toDouble();
            qDebug() << "scaleFactors :" << scaleFactors;
            if(scaleFactors == 0.0) {
                scaleFactors = defaultScaleFactors;
            }
            return scaleFactors;
        } else {
            return defaultScaleFactors;
        }
    } else {
        qWarning() << configReply.error().message();
        return defaultScaleFactors;
    }
}

static void set_auto_QT_SCALE_FACTOR() {
    const double ratio = IsWayland ? getScaleFormConfig() : get_scale_ratio();
    qDebug() << Q_FUNC_INFO << "ratio" << ratio;
    if (ratio > 0.0) {
        setenv("QT_SCALE_FACTOR", QByteArray::number(ratio).constData(), 1);
    }

    if (!qEnvironmentVariableIsSet("QT_SCALE_FACTOR")) {
        setenv("QT_AUTO_SCREEN_SCALE_FACTOR", "1", 1);
    }
}

// 初次启动时，设置一下鼠标的默认位置
void set_pointer()
{
    auto set_position = [ = ] (QPoint p) {
        Display *dpy;
        dpy = XOpenDisplay(nullptr);

        if (!dpy) return;

        Window root_window;
        root_window = XRootWindow(dpy, 0);
        XSelectInput(dpy, root_window, KeyReleaseMask);
        XWarpPointer(dpy, None, root_window, 0, 0, 0, 0, p.x(), p.y());
        XFlush(dpy);
    };

    if (!qApp->primaryScreen()) {
        QObject::connect(qApp, &QGuiApplication::primaryScreenChanged, [ = ] {
            static bool first = true;
            if (first) {
                set_position(qApp->primaryScreen()->geometry().center());
                first = false;
            }
        });
    } else {
        set_position(qApp->primaryScreen()->geometry().center());
    }
}

int main(int argc, char* argv[])
{
    // 正确加载dxcb插件
    //for qt5platform-plugins load DPlatformIntegration or DPlatformIntegrationParent
    if (!QString(qgetenv("XDG_CURRENT_DESKTOP")).toLower().startsWith("deepin")){
        setenv("XDG_CURRENT_DESKTOP", "Deepin", 1);
    }

    DGuiApplicationHelper::setAttribute(DGuiApplicationHelper::UseInactiveColorGroup, false);
    // 设置缩放，文件存在的情况下，由后端去设置，否则前端自行设置
    if (!QFile::exists("/etc/lightdm/deepin/xsettingsd.conf") || !isScaleConfigExists() || IsWayland) {
        set_auto_QT_SCALE_FACTOR();
    }

    DApplication a(argc, argv);
    // qt默认当最后一个窗口析构后，会自动退出程序，这里设置成false，防止插拔时，没有屏幕，导致进程退出
    QApplication::setQuitOnLastWindowClosed(false);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    qApp->setOrganizationName("deepin");
    qApp->setApplicationName("org.deepin.dde.lightdm-deepin-greeter");

    set_pointer();

    //注册全局事件过滤器
    AppEventFilter appEventFilter;
    a.installEventFilter(&appEventFilter);
    setAppType(APP_TYPE_LOGIN);

    DPalette pa = DGuiApplicationHelper::instance()->standardPalette(DGuiApplicationHelper::LightType);
    pa.setColor(QPalette::Normal, DPalette::WindowText, QColor("#FFFFFF"));
    pa.setColor(QPalette::Normal, DPalette::Text, QColor("#FFFFFF"));
    pa.setColor(QPalette::Normal, DPalette::AlternateBase, QColor(0, 0, 0, 76));
    pa.setColor(QPalette::Normal, DPalette::Button, QColor(255, 255, 255, 76));
    pa.setColor(QPalette::Normal, DPalette::Light, QColor(255, 255, 255, 76));
    pa.setColor(QPalette::Normal, DPalette::Dark, QColor(255, 255, 255, 76));
    pa.setColor(QPalette::Normal, DPalette::ButtonText, QColor("#FFFFFF"));
    DGuiApplicationHelper::generatePaletteColor(pa, DPalette::WindowText, DGuiApplicationHelper::LightType);
    DGuiApplicationHelper::generatePaletteColor(pa, DPalette::Text, DGuiApplicationHelper::LightType);
    DGuiApplicationHelper::generatePaletteColor(pa, DPalette::AlternateBase, DGuiApplicationHelper::LightType);
    DGuiApplicationHelper::generatePaletteColor(pa, DPalette::Button, DGuiApplicationHelper::LightType);
    DGuiApplicationHelper::generatePaletteColor(pa, DPalette::Light, DGuiApplicationHelper::LightType);
    DGuiApplicationHelper::generatePaletteColor(pa, DPalette::Dark, DGuiApplicationHelper::LightType);
    DGuiApplicationHelper::generatePaletteColor(pa, DPalette::ButtonText, DGuiApplicationHelper::LightType);
    DGuiApplicationHelper::instance()->setApplicationPalette(pa);

    DLogManager::registerConsoleAppender();

    ModulesLoader::instance().start(QThread::LowestPriority);

    const QString serviceName = "com.deepin.daemon.Accounts";
    QDBusConnectionInterface *interface = QDBusConnection::systemBus().interface();
    if (!interface->isServiceRegistered(serviceName)) {
        qWarning() << "accounts service is not registered wait...";

        QDBusServiceWatcher *serviceWatcher = new QDBusServiceWatcher(serviceName, QDBusConnection::systemBus());
        QObject::connect(serviceWatcher, &QDBusServiceWatcher::serviceUnregistered, [ = ] (const QString &service) {
            if (service == serviceName) {
                qCritical() << "ERROR, accounts service unregistered, what should I do?";
            }
        });

#ifdef ENABLE_WAITING_ACCOUNTS_SERVICE
        qDebug() << "waiting for deepin accounts service";
        QEventLoop eventLoop;
        QObject::connect(serviceWatcher, &QDBusServiceWatcher::serviceRegistered, &eventLoop, &QEventLoop::quit);
#ifdef  QT_DEBUG
        QTimer::singleShot(10000, &eventLoop, &QEventLoop::quit);
#endif
        eventLoop.exec();
        qDebug() << "service registered!";
#endif
    }

    SessionBaseModel *model = new SessionBaseModel();
    model->setAppType(Login);
    GreeterWorker *worker = new GreeterWorker(model);
    QObject::connect(&appEventFilter, &AppEventFilter::userIsActive, worker, &GreeterWorker::restartResetSessionTimer);

    /* load translation files */
    loadTranslation(model->currentUser()->locale());

    // 设置系统登录成功的加载光标
    QObject::connect(model, &SessionBaseModel::authFinished, model, [ = ](bool is_success) {
        if (is_success)
            set_rootwindow_cursor();
    });

    // 保证多个屏幕的情况下，始终只有一个屏幕显示
    PropertyGroup *property_group = new PropertyGroup(worker);
    property_group->addProperty("contentVisible");

    auto createFrame = [&](QPointer<QScreen> screen, int count) -> QWidget * {
        LoginWindow *loginFrame = new LoginWindow(model);
        // 创建Frame可能会花费数百毫秒，这个和机器性能有关，在此过程完成后，screen可能已经析构了
        // 在wayland的环境插拔屏幕或者显卡驱动有问题时可能会出现此类问题
        if (screen.isNull()) {
            loginFrame->deleteLater();
            loginFrame = nullptr;
            qWarning() << "Screen was released when the frame was created ";
            return nullptr;
        }
        loginFrame->setScreen(screen, count <= 0);
        property_group->addObject(loginFrame);
        QObject::connect(loginFrame, &LoginWindow::requestSwitchToUser, worker, &GreeterWorker::switchToUser);
        QObject::connect(loginFrame, &LoginWindow::requestSetKeyboardLayout, worker, &GreeterWorker::setKeyboardLayout);
        QObject::connect(loginFrame, &LoginWindow::requestCheckAccount, worker, &GreeterWorker::checkAccount);
        QObject::connect(loginFrame, &LoginWindow::requestStartAuthentication, worker, &GreeterWorker::startAuthentication);
        QObject::connect(loginFrame, &LoginWindow::sendTokenToAuth, worker, &GreeterWorker::sendTokenToAuth);
        QObject::connect(loginFrame, &LoginWindow::requestEndAuthentication, worker, &GreeterWorker::endAuthentication);
        QObject::connect(loginFrame, &LoginWindow::authFinished, worker, &GreeterWorker::onAuthFinished);
        QObject::connect(worker, &GreeterWorker::requestUpdateBackground, loginFrame, &LoginWindow::updateBackground);
        if (!IsWayland) {
            loginFrame->setVisible(model->visible());
        } else {
            QObject::connect(worker, &GreeterWorker::showLoginWindow, loginFrame, &LoginWindow::setVisible);
        }
        return loginFrame;
    };

    MultiScreenManager multi_screen_manager;
    multi_screen_manager.register_for_mutil_screen(createFrame);
    QObject::connect(model, &SessionBaseModel::visibleChanged, &multi_screen_manager, &MultiScreenManager::startRaiseContentFrame);

#if defined(DSS_CHECK_ACCESSIBILITY) && defined(QT_DEBUG)
    AccessibilityCheckerEx checker;
    checker.addIgnoreClasses(QStringList()
                             << "Dtk::Widget::DBlurEffectWidget");
    checker.setOutputFormat(DAccessibilityChecker::FullFormat);
    checker.start();
#endif
    if (!IsWayland) {
        model->setVisible(true);
    }

    return a.exec();
}
