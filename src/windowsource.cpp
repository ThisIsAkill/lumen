#include "windowsource.h"
#include "fuzzy.h"
#include "launcher.h"
#include "windowtiling.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QtConcurrent>

#include <fcntl.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcWindows, "lumen.windows", QtWarningMsg)

namespace
{

constexpr int ThumbnailWidth = 640;
constexpr int MaxThumbnails = 16;

QString thumbnailDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/lumen-thumbnails");
}

// Reads the image KWin writes into the pipe (in a worker thread). It reads
// exactly `size` bytes: the pipe never reports its end, because the pending
// D-Bus call keeps a copy of its write end open.
QByteArray readExactly(int fd, qsizetype size)
{
    QByteArray data(size, Qt::Uninitialized);
    qsizetype got = 0;
    while (got < size) {
        const ssize_t n = ::read(fd, data.data() + got, size_t(size - got));
        if (n > 0)
            got += n;
        else if (n == 0 || errno != EINTR)
            break;
    }
    ::close(fd);
    data.resize(got);
    return data;
}


} // namespace

WindowSource::WindowSource(Launcher *launcher)
    : Source(launcher)
{
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Windows"), this, QDBusConnection::ExportScriptableSlots);
    QDir().mkpath(thumbnailDir());
}

void WindowSource::sessionStarted()
{
    m_windows.clear();
}

void WindowSource::requestWindows()
{
    // Newest first: windowList() is in stacking order, bottom to top.
    WindowTiling::runScript(QStringLiteral("window-list"), QStringLiteral(R"js(
const out = [];
for (const w of workspace.windowList()) {
    if (!w.normalWindow || w.skipTaskbar || w.skipSwitcher || w.resourceClass === "lumen")
        continue;
    out.unshift({id: w.internalId.toString(), caption: w.caption, app: w.resourceClass,
                 icon: w.desktopFileName, minimized: w.minimized, active: w === workspace.activeWindow});
}
callDBus("io.github.lumen", "/Windows", "io.github.lumen.Windows", "report", JSON.stringify(out));
)js"));
}

void WindowSource::report(const QString &json)
{
    const QHash<QString, QString> thumbnails = [this] {
        QHash<QString, QString> t;
        for (const Window &w : std::as_const(m_windows))
            t.insert(w.id, w.thumbnail);
        return t;
    }();
    m_windows.clear();
    for (const QJsonValue &v : QJsonDocument::fromJson(json.toUtf8()).array()) {
        const QJsonObject o = v.toObject();
        Window w{o.value(QStringLiteral("id")).toString(), o.value(QStringLiteral("caption")).toString(),
                 o.value(QStringLiteral("app")).toString(), o.value(QStringLiteral("icon")).toString(),
                 o.value(QStringLiteral("minimized")).toBool(), o.value(QStringLiteral("active")).toBool(), {}};
        w.thumbnail = thumbnails.value(w.id);
        m_windows << w;
    }
    publish();
    captureThumbnails();
}

void WindowSource::captureThumbnails()
{
    int started = 0;
    for (const Window &w : std::as_const(m_windows)) {
        if (!w.thumbnail.isEmpty() || w.minimized || started == MaxThumbnails)
            continue;
        ++started;
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) != 0)
            return;
        auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/org/kde/KWin/ScreenShot2"),
                                                  QStringLiteral("org.kde.KWin.ScreenShot2"), QStringLiteral("CaptureWindow"));
        msg.setArguments({w.id, QVariantMap{{QStringLiteral("include-decoration"), true}, {QStringLiteral("native-resolution"), false}},
                          QVariant::fromValue(QDBusUnixFileDescriptor(fds[1]))});
        ::close(fds[1]); // the message holds its own copy
        const int readFd = fds[0];
        auto *reply = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg, 3000), this);
        const QString id = w.id;
        connect(reply, &QDBusPendingCallWatcher::finished, this, [this, reply, readFd, id] {
            reply->deleteLater();
            // D-Bus maps arrive wrapped in a QDBusArgument.
            const QVariantMap info = qdbus_cast<QVariantMap>(reply->reply().arguments().value(0));
            const int width = info.value(QStringLiteral("width")).toInt();
            const int height = info.value(QStringLiteral("height")).toInt();
            const int stride = info.value(QStringLiteral("stride")).toInt();
            const auto format = QImage::Format(info.value(QStringLiteral("format")).toInt());
            if (reply->isError() || width <= 0 || height <= 0 || stride <= 0) {
                // Not allowed (no desktop entry permission), or the window went away.
                qCDebug(lcWindows) << "no thumbnail for" << id << reply->error().name() << reply->error().message();
                ::close(readFd);
                return;
            }
            auto *data = new QFutureWatcher<QByteArray>(this);
            connect(data, &QFutureWatcher<QByteArray>::finished, this, [this, data, id, width, height, stride, format] {
                const QByteArray pixels = data->result();
                data->deleteLater();
                if (pixels.size() < qsizetype(stride) * height)
                    return;
                const QImage image = QImage(reinterpret_cast<const uchar *>(pixels.constData()), width, height, stride, format)
                                         .scaledToWidth(qMin(width, ThumbnailWidth), Qt::SmoothTransformation);
                const QString file = thumbnailDir() + u'/' + QString(id).remove(u'{').remove(u'}') + QStringLiteral(".png");
                if (!image.save(file))
                    return;
                for (Window &win : m_windows)
                    if (win.id == id)
                        win.thumbnail = file;
                publish();
            });
            data->setFuture(QtConcurrent::run(readExactly, readFd, qsizetype(stride) * height));
        });
    }
}

void WindowSource::windowScript(const QString &id, const QString &body)
{
    WindowTiling::runScript(QStringLiteral("window-action"), QStringLiteral(R"js(
for (const w of workspace.windowList()) {
    if (w.internalId.toString() === %1) {
        %2
    }
}
)js")
                                                                  .arg(WindowTiling::jsString(id), body));
}

void WindowSource::query(const QString &text)
{
    m_query = text.trimmed();
    if (m_windows.isEmpty())
        requestWindows(); // the answer arrives in report()
    publish();
}

void WindowSource::publish()
{
    QList<Item> items;
    Launcher *l = launcher();
    for (int i = 0; i < m_windows.size(); ++i) {
        const Window &w = m_windows.at(i);
        qreal score = m_query.isEmpty() ? 1000 - i : std::max(fuzzyScore(m_query, w.caption), fuzzyScore(m_query, w.app));
        if (score <= 0)
            continue;
        Item item;
        item.id = QStringLiteral("window:") + w.id;
        item.title = w.caption;
        item.subtitle = w.app;
        item.kind = w.active ? tr("Active") : w.minimized ? tr("Minimized") : QString();
        item.icon = QIcon::fromTheme(w.icon, QIcon::fromTheme(w.app.toLower(), QIcon::fromTheme(QStringLiteral("window"))));
        item.previewImage = w.thumbnail;
        item.previewText = w.thumbnail.isEmpty() ? w.caption : QString();
        item.score = score;
        const QString id = w.id;
        item.actions << Action{tr("Switch to Window"), {}, {}, [this, l, id] {
            l->runAfterHide([this, id] { windowScript(id, QStringLiteral("w.minimized = false; workspace.activeWindow = w;")); });
        }};
        item.actions << Action{tr("Close Window"), QStringLiteral("window-close"), QKeySequence(Qt::CTRL | Qt::Key_W), [this, id] {
            windowScript(id, QStringLiteral("w.closeWindow();"));
            m_windows.removeIf([&](const Window &win) { return win.id == id; });
            publish();
        }, true};
        item.actions << Action{tr("Minimize Window"), QStringLiteral("window-minimize"), QKeySequence(Qt::CTRL | Qt::Key_M),
                               [this, id] { windowScript(id, QStringLiteral("w.minimized = true;")); }};
        items << item;
    }
    if (items.isEmpty() && m_windows.isEmpty()) {
        Item none;
        none.title = tr("No windows");
        none.subtitle = tr("Listing windows needs KWin");
        none.icon = QIcon::fromTheme(QStringLiteral("window"));
        items << none;
    }
    Q_EMIT resultsReady(items);
}
