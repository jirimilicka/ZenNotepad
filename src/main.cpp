#include "mainwindow.h"
#include "translations.h"

#include <QTimer>

int main(int argc, char **argv)
{
    // The KDE platform theme plugin costs ~100 ms of startup; the generic theme with the
    // Breeze style looks the same for our few widgets. File dialogs go through the portal.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORMTHEME"))
        qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QApplication app(argc, argv);
    if (!qEnvironmentVariableIsSet("QT_STYLE_OVERRIDE"))
        QApplication::setStyle(QStringLiteral("breeze"));
    QCoreApplication::setOrganizationName(QStringLiteral("zen-notepad"));
    QCoreApplication::setApplicationName(QStringLiteral("zen-notepad"));
    setUiLanguage(QSettings().value("language", QStringLiteral("auto")).toString());
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.jirimilicka.zen_notepad"));
    MainWindow w;
    QStringList args = app.arguments();
    if (args.size() > 1)
        w.openPath(args.at(1));
    w.show();
    if (qEnvironmentVariableIsSet("ZEN_NOTEPAD_BENCH"))  // startup benchmark: quit after first frame
        QTimer::singleShot(0, &app, [&] { w.repaint(); app.quit(); });
    return app.exec();
}
