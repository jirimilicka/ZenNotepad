#include "portal.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QUrl>
#include <QWidget>

namespace {
class ResponseWaiter : public QObject {
    Q_OBJECT
public:
    QEventLoop loop;
    bool answered = false;
    QStringList uris;
public Q_SLOTS:
    void response(uint code, const QVariantMap &results)
    {
        answered = true;
        if (code == 0)
            uris = results.value(QStringLiteral("uris")).toStringList();
        loop.quit();
    }
};

bool portalDialog(QWidget *parent, bool save, const QString &currentPath, QString &result)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    QString token = QStringLiteral("zennotepad%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1).replace(QLatin1Char('.'), QLatin1Char('_'));
    QString reqPath = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    ResponseWaiter w;
    bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), reqPath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), &w,
                SLOT(response(uint, QVariantMap)));

    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), token);
    opts.insert(QStringLiteral("modal"), true);
    QFileInfo fi(currentPath);
    QString folder = currentPath.isEmpty() ? QDir::homePath() : fi.absolutePath();
    QByteArray f = QFile::encodeName(folder);
    f.append('\0');
    opts.insert(QStringLiteral("current_folder"), f);
    if (save && !currentPath.isEmpty())
        opts.insert(QStringLiteral("current_name"), fi.fileName());

    QString handle;
    if (QGuiApplication::platformName() == QLatin1String("xcb") && parent)
        handle = QStringLiteral("x11:%1").arg(parent->window()->winId(), 0, 16);

    QDBusMessage m = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.FileChooser"), save ? QStringLiteral("SaveFile") : QStringLiteral("OpenFile"));
    m << handle << (save ? QObject::tr("Save As") : QObject::tr("Open")) << opts;
    QDBusMessage reply = bus.call(m, QDBus::Block, 5000);
    if (reply.type() != QDBusMessage::ReplyMessage)
        return false;
    if (parent)
        parent->window()->setEnabled(false);
    w.loop.exec();
    if (parent)
        parent->window()->setEnabled(true);
    result.clear();
    if (!w.uris.isEmpty())
        result = QUrl(w.uris.first()).toLocalFile();
    return true;
}
} // namespace

QString chooseFile(QWidget *parent, bool save, const QString &currentPath)
{
    QString r;
    if (portalDialog(parent, save, currentPath, r))
        return r;
    QString dir = currentPath.isEmpty() ? QDir::homePath() : currentPath;
    return save ? QFileDialog::getSaveFileName(parent, QString(), dir)
                : QFileDialog::getOpenFileName(parent, QString(), QFileInfo(dir).absolutePath());
}

#include "portal.moc"
