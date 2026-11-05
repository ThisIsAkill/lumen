#include "clipboardhistory.h"
#include "plural.h"
#include "launcher.h"
#include "pastestack.h"
#include "screentools.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QMimeData>
#include <QSaveFile>
#include <QStandardPaths>

namespace
{

constexpr int MaxTextLength = 200000; // longer copies are not kept
constexpr int MaxBacklog = 100;        // older images read at start-up
constexpr int RecognizeTimeout = 60000;
const QString PasswordHint = QStringLiteral("x-kde-passwordManagerHint");

bool onWayland()
{
    return QGuiApplication::platformName() == QLatin1String("wayland")
        && !QStandardPaths::findExecutable(QStringLiteral("wl-paste")).isEmpty();
}

// Runs wl-paste with args and hands its output to done (not called on failure).
void wlPaste(QObject *context, const QStringList &args, std::function<void(const QByteArray &)> done)
{
    auto *proc = new QProcess(context);
    QObject::connect(proc, &QProcess::finished, context, [proc, done](int code) {
        if (code == 0)
            done(proc->readAllStandardOutput());
        proc->deleteLater();
    });
    proc->start(QStringLiteral("wl-paste"), args);
    QTimer::singleShot(2000, proc, [proc] { proc->kill(); }); // an unresponsive clipboard owner
}

// Copying is the secondary action (Ctrl+Return) when Enter pastes.
QKeySequence copyKey(const Launcher *launcher)
{
    return launcher->canPaste() ? QKeySequence(Qt::CTRL | Qt::Key_Return) : QKeySequence();
}

QString relativeTime(const QDateTime &time)
{
    const qint64 secs = time.secsTo(QDateTime::currentDateTime());
    if (secs < 60)
        return QObject::tr("Just now");
    if (secs < 3600)
        return QObject::tr("%1 min ago").arg(secs / 60);
    if (time.date() == QDate::currentDate())
        return QLocale().toString(time.time(), QLocale::ShortFormat);
    if (time.date() == QDate::currentDate().addDays(-1))
        return QObject::tr("Yesterday");
    return QLocale().toString(time.date(), QLocale::ShortFormat);
}

} // namespace

ClipboardHistory::ClipboardHistory(QObject *parent)
    : QObject(parent)
    , m_dir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/lumen/clipboard"))
{
    QDir().mkpath(m_dir);
    load();
    // Images copied before (newest first) are read once Lumen is idle.
    for (const Entry &e : std::as_const(m_entries))
        if (!e.imageFile.isEmpty() && !e.recognized && m_toRecognize.size() < MaxBacklog)
            m_toRecognize << e.imageFile;
    QTimer::singleShot(5000, this, [this] {
        if (m_recognizer.isEmpty() && !m_recognizerSet)
            setRecognizer(defaultRecognizer());
    });
    m_fetchDelay.setSingleShot(true);
    m_fetchDelay.setInterval(60); // copies often change the selection several times in a row
    connect(&m_fetchDelay, &QTimer::timeout, this, &ClipboardHistory::fetchWayland);
    startWatching();
}

ClipboardHistory::~ClipboardHistory()
{
    stopWatching();
}

void ClipboardHistory::setEnabled(bool enabled)
{
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    enabled ? startWatching() : stopWatching();
}

void ClipboardHistory::setLimit(int limit)
{
    m_limit = qMax(10, limit);
    trim();
}

void ClipboardHistory::startWatching()
{
    if (!m_enabled)
        return;
    if (onWayland()) {
        m_watcher = new QProcess(this);
        // wl-paste runs the command (here a harmless printf) on every change.
        m_watcher->start(QStringLiteral("wl-paste"), {QStringLiteral("--watch"), QStringLiteral("printf"), QStringLiteral("x")});
        connect(m_watcher, &QProcess::readyReadStandardOutput, this, [this] {
            m_watcher->readAllStandardOutput();
            m_fetchDelay.start();
        });
    } else {
        connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, &ClipboardHistory::readQtClipboard);
    }
}

void ClipboardHistory::stopWatching()
{
    if (m_watcher) {
        m_watcher->kill();
        m_watcher->waitForFinished(500);
        delete m_watcher;
        m_watcher = nullptr;
    }
    disconnect(QGuiApplication::clipboard(), nullptr, this, nullptr);
}

void ClipboardHistory::fetchWayland()
{
    wlPaste(this, {QStringLiteral("--list-types")}, [this](const QByteArray &out) {
        const QStringList types = QString::fromUtf8(out).split(u'\n', Qt::SkipEmptyParts);
        if (types.contains(PasswordHint))
            return;
        if (types.contains(QLatin1String("image/png")) && !types.contains(QLatin1String("text/uri-list"))) {
            wlPaste(this, {QStringLiteral("--type"), QStringLiteral("image/png")},
                    [this](const QByteArray &png) { addImage(QImage::fromData(png, "PNG")); });
            return;
        }
        for (const char *t : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"}) {
            if (types.contains(QLatin1String(t))) {
                wlPaste(this, {QStringLiteral("--no-newline"), QStringLiteral("--type"), QLatin1String(t)},
                        [this](const QByteArray &text) { addText(QString::fromUtf8(text)); });
                return;
            }
        }
    });
}

void ClipboardHistory::readQtClipboard()
{
    const QMimeData *data = QGuiApplication::clipboard()->mimeData();
    if (!data || data->hasFormat(PasswordHint))
        return;
    if (data->hasImage() && !data->hasUrls())
        addImage(qvariant_cast<QImage>(data->imageData()));
    else if (data->hasText())
        addText(data->text());
}

void ClipboardHistory::addText(const QString &text)
{
    if (text.trimmed().isEmpty() || text.size() > MaxTextLength)
        return;
    addEntry({text, {}, {}, QDateTime::currentDateTime(), false});
}

void ClipboardHistory::addImage(const QImage &image)
{
    if (image.isNull())
        return;
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    const QString name = QString::fromLatin1(QCryptographicHash::hash(png, QCryptographicHash::Sha1).toHex()) + QStringLiteral(".png");
    QSaveFile f(QDir(m_dir).filePath(name));
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(png);
    if (!f.commit())
        return;
    addEntry({{}, name, image.size(), QDateTime::currentDateTime(), false});
}

void ClipboardHistory::addEntry(const Entry &entry)
{
    // Copying something again moves it to the top and keeps its pin (and
    // the text found in it).
    Entry fresh = entry;
    for (qsizetype i = 0; i < m_entries.size(); ++i) {
        const Entry &e = m_entries.at(i);
        if (e.text == entry.text && e.imageFile == entry.imageFile) {
            fresh.pinned = e.pinned;
            fresh.imageText = e.imageText;
            fresh.recognized = e.recognized;
            m_entries.removeAt(i);
            break;
        }
    }
    m_entries.prepend(fresh);
    trim();
    save();
    Q_EMIT added(fresh);
    Q_EMIT changed();
    if (!fresh.imageFile.isEmpty() && !fresh.recognized) {
        m_toRecognize.prepend(fresh.imageFile);
        recognizeNext();
    }
}

void ClipboardHistory::trim()
{
    int unpinned = 0;
    for (qsizetype i = 0; i < m_entries.size();) {
        const Entry &e = m_entries.at(i);
        if (!e.pinned && ++unpinned > m_limit) {
            if (!e.imageFile.isEmpty())
                QFile::remove(imagePath(e));
            m_entries.removeAt(i);
        } else {
            ++i;
        }
    }
}

QString ClipboardHistory::imagePath(const Entry &entry) const
{
    return QDir(m_dir).filePath(entry.imageFile);
}

void ClipboardHistory::remove(int index)
{
    if (index < 0 || index >= m_entries.size())
        return;
    const Entry e = m_entries.takeAt(index);
    if (!e.imageFile.isEmpty())
        QFile::remove(imagePath(e));
    save();
    Q_EMIT changed();
}

void ClipboardHistory::setPinned(int index, bool pinned)
{
    if (index < 0 || index >= m_entries.size())
        return;
    m_entries[index].pinned = pinned;
    save();
    Q_EMIT changed();
}

void ClipboardHistory::clearUnpinned()
{
    for (qsizetype i = m_entries.size() - 1; i >= 0; --i)
        if (!m_entries.at(i).pinned)
            remove(int(i));
}

void ClipboardHistory::load()
{
    QFile f(QDir(m_dir).filePath(QStringLiteral("history.json")));
    if (!f.open(QIODevice::ReadOnly))
        return;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.text = o.value(QStringLiteral("text")).toString();
        e.imageFile = o.value(QStringLiteral("image")).toString();
        e.imageSize = QSize(o.value(QStringLiteral("width")).toInt(), o.value(QStringLiteral("height")).toInt());
        e.time = QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("time")).toInteger());
        e.pinned = o.value(QStringLiteral("pinned")).toBool();
        e.imageText = o.value(QStringLiteral("imageText")).toString();
        e.recognized = o.value(QStringLiteral("recognized")).toBool();
        if (!e.imageFile.isEmpty() && !QFile::exists(imagePath(e)))
            continue;
        m_entries << e;
    }
}

void ClipboardHistory::save() const
{
    QJsonArray list;
    for (const Entry &e : m_entries) {
        QJsonObject o{{QStringLiteral("time"), e.time.toSecsSinceEpoch()}, {QStringLiteral("pinned"), e.pinned}};
        if (e.imageFile.isEmpty()) {
            o.insert(QStringLiteral("text"), e.text);
        } else {
            o.insert(QStringLiteral("image"), e.imageFile);
            o.insert(QStringLiteral("width"), e.imageSize.width());
            o.insert(QStringLiteral("height"), e.imageSize.height());
            if (e.recognized) {
                o.insert(QStringLiteral("recognized"), true);
                o.insert(QStringLiteral("imageText"), e.imageText);
            }
        }
        list << o;
    }
    // The history may hold private text: readable by the user only.
    QSaveFile f(QDir(m_dir).filePath(QStringLiteral("history.json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        f.write(QJsonDocument(list).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

// --------------------------------------------------------- recognition

QStringList ClipboardHistory::defaultRecognizer()
{
    const KConfigGroup config = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Clipboard"));
    if (!config.readEntry("RecognizeText", true) || QStandardPaths::findExecutable(QStringLiteral("tesseract")).isEmpty())
        return {};
    // Only with the language data installed; otherwise every image would fail.
    const QString languages = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("OCR")).readEntry("Languages", QStringLiteral("eng"));
    const QStringList installed = ScreenTools::ocrLanguages();
    for (const QString &lang : languages.split(u'+'))
        if (!installed.contains(lang))
            return {};
    QStringList command{QStringLiteral("tesseract"), QStringLiteral("{image}"), QStringLiteral("-"), QStringLiteral("-l"), languages};
    if (!QStandardPaths::findExecutable(QStringLiteral("nice")).isEmpty())
        command = QStringList{QStringLiteral("nice"), QStringLiteral("-n"), QStringLiteral("19")} + command;
    return command;
}

void ClipboardHistory::setRecognizer(const QStringList &command)
{
    m_recognizerSet = true;
    m_recognizer = command;
    recognizeNext();
}

void ClipboardHistory::recognizeNext()
{
    if (m_recognizing || m_recognizer.isEmpty())
        return;
    while (!m_toRecognize.isEmpty()) {
        const QString file = m_toRecognize.takeFirst();
        const auto it = std::find_if(m_entries.cbegin(), m_entries.cend(), [&](const Entry &e) { return e.imageFile == file; });
        if (it == m_entries.cend() || it->recognized)
            continue; // deleted meanwhile, or done
        QStringList command = m_recognizer;
        command.replaceInStrings(QStringLiteral("{image}"), QDir(m_dir).filePath(file));
        m_recognizing = new QProcess(this);
        connect(m_recognizing, &QProcess::finished, this, [this, file](int code, QProcess::ExitStatus status) {
            const QString text = QString::fromUtf8(m_recognizing->readAllStandardOutput()).trimmed();
            m_recognizing->deleteLater();
            m_recognizing = nullptr;
            for (Entry &e : m_entries) {
                if (e.imageFile == file) {
                    e.recognized = true;
                    e.imageText = status == QProcess::NormalExit && code == 0 ? text : QString();
                    save();
                    Q_EMIT changed();
                    break;
                }
            }
            recognizeNext();
        });
        QTimer::singleShot(RecognizeTimeout, m_recognizing, [proc = m_recognizing] { proc->kill(); });
        m_recognizing->start(command.first(), command.mid(1));
        return;
    }
}

// ---------------------------------------------------------------- view

ClipboardSource::ClipboardSource(Launcher *launcher, ClipboardHistory *history)
    : Source(launcher)
    , m_history(history)
{
    // Refresh the open view when something is copied, pinned or deleted.
    connect(history, &ClipboardHistory::changed, this, [this] {
        if (this->launcher()->currentSource() == this)
            query(m_lastQuery);
    });
}

void ClipboardSource::query(const QString &text)
{
    m_lastQuery = text;
    const QString q = text.trimmed();
    QList<Item> pinned, recent;
    const QList<ClipboardHistory::Entry> entries = m_history->entries();
    Launcher *l = launcher();
    ClipboardHistory *history = m_history;

    for (int i = 0; i < entries.size(); ++i) {
        const ClipboardHistory::Entry &e = entries.at(i);
        const bool image = !e.imageFile.isEmpty();
        if (!q.isEmpty() && !(image ? e.imageText : e.text).contains(q, Qt::CaseInsensitive))
            continue;
        Item item;
        item.id = QStringLiteral("clip:%1").arg(i);
        if (image) {
            const QString path = m_history->imagePath(e);
            // Named by the text in it, when it has any.
            const QString firstWords = e.imageText.simplified().left(120);
            item.title = firstWords.isEmpty() ? tr("Image") : firstWords;
            item.subtitle = tr("Image · %1 × %2 · %3").arg(e.imageSize.width()).arg(e.imageSize.height()).arg(relativeTime(e.time));
            item.icon = QIcon(path);
            item.filePath = path;
            if (l->canPaste())
                item.actions << Action{tr("Paste"), {}, {}, [l, path] { l->pasteImage(path); }};
            item.actions << Action{tr("Copy to Clipboard"), QStringLiteral("edit-copy"), copyKey(l), [l, path] { l->copyImage(path); }};
            if (!e.imageText.isEmpty()) {
                const QString text = e.imageText;
                item.actions << Action{tr("Copy Text in Image"), QStringLiteral("edit-select-text"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
                                       [l, text] { l->copyText(text); }};
            }
        } else {
            item.title = e.text.simplified().left(200);
            item.subtitle = plural(e.text.size(), tr("%1 character · %2"), tr("%1 characters · %2")).arg(relativeTime(e.time));
            item.icon = QIcon::fromTheme(e.text.startsWith(QLatin1String("http")) ? QStringLiteral("link")
                                                                                 : QStringLiteral("text-plain"));
            const QString content = e.text;
            item.previewText = e.text;
            if (l->canPaste())
                item.actions << Action{tr("Paste"), {}, {}, [l, content] { l->pasteText(content); }};
            item.actions << Action{tr("Copy to Clipboard"), QStringLiteral("edit-copy"), copyKey(l), [l, content] { l->copyText(content); }};
        }
        item.kind = e.pinned ? tr("Pinned") : QString();
        item.actions << Action{tr("Add to Paste Stack"), QStringLiteral("view-list-text"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [l, e] {
            if (!l->pasteStack()->isActive())
                l->pasteStack()->start();
            l->pasteStack()->add(e);
            l->notify(tr("Added to the paste stack"));
        }, true};
        const bool pin = !e.pinned;
        item.actions << Action{pin ? tr("Pin") : tr("Unpin"), QStringLiteral("pin"), QKeySequence(Qt::CTRL | Qt::Key_P),
                               [history, i, pin] { history->setPinned(i, pin); }, true};
        item.actions << Action{tr("Delete"), QStringLiteral("edit-delete"), QKeySequence(Qt::CTRL | Qt::Key_Backspace),
                               [history, i] { history->remove(i); }, true};
        item.actions << Action{tr("Delete All Unpinned"), QStringLiteral("edit-clear-history"),
                               QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backspace), [history] { history->clearUnpinned(); }, true};
        (e.pinned ? pinned : recent) << item;
    }
    // Keep history order (newest first) rather than sorting by score.
    QList<Item> items = pinned + recent;
    for (int i = 0; i < items.size(); ++i)
        items[i].score = 1000 - i;
    Q_EMIT resultsReady(items);
}
