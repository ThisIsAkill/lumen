#include "emojisource.h"
#include "fuzzy.h"
#include "launcher.h"

#include <QFile>
#include <QLocale>
#include <QPainter>
#include <QStandardPaths>
#include <QtEndian>

#include <algorithm>

namespace
{

constexpr int ResultLimit = 120;

// The entries are little-endian: a count, then per emoji its character and
// name (length-prefixed UTF-8), a category number and a keyword list.
class Reader
{
public:
    explicit Reader(const QByteArray &data)
        : m_data(data)
    {
    }
    bool ok() const { return m_ok; }
    quint32 number()
    {
        if (m_pos + 4 > m_data.size()) {
            m_ok = false;
            return 0;
        }
        const quint32 v = qFromLittleEndian<quint32>(m_data.constData() + m_pos);
        m_pos += 4;
        return v;
    }
    QString text()
    {
        const quint32 length = number();
        if (!m_ok || m_pos + length > quint64(m_data.size())) {
            m_ok = false;
            return {};
        }
        const QString s = QString::fromUtf8(m_data.constData() + m_pos, length);
        m_pos += length;
        return s;
    }

private:
    QByteArray m_data;
    qsizetype m_pos = 0;
    bool m_ok = true;
};

QString dictionaryFile()
{
    const QString dir = QStringLiteral("plasma/emoji/");
    // (A named list: appending to the temporary would leave the loop reading freed memory.)
    QStringList languages = QLocale().uiLanguages(QLocale::TagSeparator::Underscore);
    languages << QStringLiteral("en");
    for (const QString &lang : std::as_const(languages)) {
        for (const QString &candidate : {lang, lang.section(u'_', 0, 0)}) {
            const QString file = QStandardPaths::locate(QStandardPaths::GenericDataLocation, dir + candidate + QStringLiteral(".dict"));
            if (!file.isEmpty())
                return file;
        }
    }
    return {};
}

QIcon emojiIcon(const QString &character)
{
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    QFont font(QStringLiteral("emoji"));
    font.setPixelSize(48);
    p.setFont(font);
    p.drawText(pixmap.rect(), Qt::AlignCenter, character);
    return QIcon(pixmap);
}

} // namespace

QList<EmojiSource::Emoji> EmojiSource::load(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    Reader r(qUncompress(f.readAll()));
    QList<Emoji> list;
    const quint32 count = r.number();
    for (quint32 i = 0; r.ok() && i < count; ++i) {
        Emoji e;
        e.character = r.text();
        e.name = r.text();
        r.number(); // category
        const quint32 keywords = r.number();
        for (quint32 k = 0; r.ok() && k < keywords; ++k)
            e.keywords << r.text();
        if (r.ok())
            list << e;
    }
    return list;
}

void EmojiSource::query(const QString &text)
{
    if (m_emoji.isEmpty())
        m_emoji = load(dictionaryFile());
    const QString q = text.trimmed();

    struct Match {
        const Emoji *emoji;
        qreal score;
    };
    QList<Match> matches;
    for (const Emoji &e : std::as_const(m_emoji)) {
        qreal score = q.isEmpty() ? 1 : fuzzyScore(q, e.name);
        for (const QString &k : e.keywords)
            score = std::max(score, fuzzyScore(q, k) * 0.9);
        if (score > 0)
            matches << Match{&e, score};
    }
    std::stable_sort(matches.begin(), matches.end(), [](const Match &a, const Match &b) { return a.score > b.score; });
    if (matches.size() > ResultLimit)
        matches.resize(ResultLimit);

    QList<Item> items;
    Launcher *l = launcher();
    for (int i = 0; i < matches.size(); ++i) {
        const Emoji &e = *matches.at(i).emoji;
        Item item;
        item.id = QStringLiteral("emoji:") + e.character;
        item.title = e.name.left(1).toUpper() + e.name.mid(1);
        item.subtitle = e.keywords.mid(0, 6).join(QStringLiteral(", "));
        item.icon = emojiIcon(e.character);
        item.score = 1000 - i;
        const QString c = e.character;
        if (l->canPaste())
            item.actions << Action{tr("Paste"), {}, {}, [l, c] { l->pasteText(c); }};
        item.actions << Action{tr("Copy to Clipboard"), QStringLiteral("edit-copy"),
                               l->canPaste() ? QKeySequence(Qt::CTRL | Qt::Key_Return) : QKeySequence(),
                               [l, c] { l->copyText(c); }};
        items << item;
    }
    Q_EMIT resultsReady(items);
}
