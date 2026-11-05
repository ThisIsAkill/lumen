#include "transforms.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QUrl>
#include <QUuid>

#include <algorithm>

namespace Transforms
{

namespace
{

// Words of an identifier or sentence: "helloWorld-foo_bar baz" → hello, world, foo, bar, baz.
QStringList words(const QString &text)
{
    QString spaced;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (i > 0 && c.isUpper() && text.at(i - 1).isLower())
            spaced += u' ';
        spaced += c.isLetterOrNumber() ? c : QChar(u' ');
    }
    return spaced.toLower().split(u' ', Qt::SkipEmptyParts);
}

QString capitalised(const QString &word)
{
    return word.isEmpty() ? word : word.left(1).toUpper() + word.mid(1);
}

QStringList lines(const QString &text)
{
    return text.split(u'\n');
}

std::optional<QString> some(const QString &text)
{
    return text;
}

const QString LoremText = QStringLiteral(
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
    "Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure "
    "dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non "
    "proident, sunt in culpa qui officia deserunt mollit anim id est laborum.");

} // namespace

const QList<Transform> &all()
{
    static const QList<Transform> list{
        {QStringLiteral("upper"), QStringLiteral("UPPERCASE"), {QStringLiteral("upper"), QStringLiteral("caps")},
         [](const QString &t) { return some(t.toUpper()); }},
        {QStringLiteral("lower"), QStringLiteral("lowercase"), {QStringLiteral("lower")}, [](const QString &t) { return some(t.toLower()); }},
        {QStringLiteral("title"), QStringLiteral("Title Case"), {QStringLiteral("title"), QStringLiteral("capitalize")},
         [](const QString &t) {
             QString out = t.toLower();
             for (qsizetype i = 0; i < out.size(); ++i)
                 if (out.at(i).isLetter() && (i == 0 || (!out.at(i - 1).isLetterOrNumber() && out.at(i - 1) != u'\''))) // not "Don'T"
                     out[i] = out.at(i).toUpper();
             return some(out);
         }},
        {QStringLiteral("sentence"), QStringLiteral("Sentence case"), {QStringLiteral("sentence")},
         [](const QString &t) {
             QString out = t.toLower();
             bool start = true;
             for (QChar &c : out) {
                 if (start && c.isLetter()) {
                     c = c.toUpper();
                     start = false;
                 } else if (c == u'.' || c == u'!' || c == u'?' || c == u'\n') {
                     start = true;
                 }
             }
             return some(out);
         }},
        {QStringLiteral("camel"), QStringLiteral("camelCase"), {QStringLiteral("camel")},
         [](const QString &t) {
             const QStringList w = words(t);
             QString out = w.value(0);
             for (const QString &x : w.mid(1))
                 out += capitalised(x);
             return some(out);
         }},
        {QStringLiteral("pascal"), QStringLiteral("PascalCase"), {QStringLiteral("pascal")},
         [](const QString &t) {
             QString out;
             for (const QString &x : words(t))
                 out += capitalised(x);
             return some(out);
         }},
        {QStringLiteral("snake"), QStringLiteral("snake_case"), {QStringLiteral("snake")}, [](const QString &t) { return some(words(t).join(u'_')); }},
        {QStringLiteral("kebab"), QStringLiteral("kebab-case / slug"), {QStringLiteral("kebab"), QStringLiteral("slug")},
         [](const QString &t) { return some(words(t).join(u'-')); }},
        {QStringLiteral("constant"), QStringLiteral("CONSTANT_CASE"), {QStringLiteral("constant")},
         [](const QString &t) { return some(words(t).join(u'_').toUpper()); }},
        {QStringLiteral("trim"), QStringLiteral("Trim Whitespace"), {QStringLiteral("trim"), QStringLiteral("strip")},
         [](const QString &t) {
             QStringList out;
             for (const QString &l : lines(t))
                 out << l.trimmed();
             return some(out.join(u'\n').trimmed());
         }},
        {QStringLiteral("oneline"), QStringLiteral("Join Lines"), {QStringLiteral("join"), QStringLiteral("one line"), QStringLiteral("unwrap")},
         [](const QString &t) { return some(t.simplified()); }},
        {QStringLiteral("sort"), QStringLiteral("Sort Lines"), {QStringLiteral("sort")},
         [](const QString &t) {
             QStringList l = lines(t);
             std::sort(l.begin(), l.end(), [](const QString &a, const QString &b) { return QString::localeAwareCompare(a, b) < 0; });
             return some(l.join(u'\n'));
         }},
        {QStringLiteral("unique"), QStringLiteral("Remove Duplicate Lines"), {QStringLiteral("unique"), QStringLiteral("dedupe")},
         [](const QString &t) {
             QStringList l = lines(t);
             l.removeDuplicates();
             return some(l.join(u'\n'));
         }},
        {QStringLiteral("reverse"), QStringLiteral("Reverse Lines"), {QStringLiteral("reverse")},
         [](const QString &t) {
             QStringList l = lines(t);
             std::reverse(l.begin(), l.end());
             return some(l.join(u'\n'));
         }},
        {QStringLiteral("base64"), QStringLiteral("Base64 Encode"), {QStringLiteral("base64"), QStringLiteral("encode")},
         [](const QString &t) { return some(QString::fromLatin1(t.toUtf8().toBase64())); }},
        {QStringLiteral("unbase64"), QStringLiteral("Base64 Decode"), {QStringLiteral("base64 decode"), QStringLiteral("decode")},
         [](const QString &t) -> std::optional<QString> {
             const auto r = QByteArray::fromBase64Encoding(t.trimmed().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
             if (!r || r.decoded.isEmpty())
                 return std::nullopt;
             return QString::fromUtf8(r.decoded);
         }},
        {QStringLiteral("url"), QStringLiteral("URL Encode"), {QStringLiteral("url encode"), QStringLiteral("percent")},
         [](const QString &t) { return some(QString::fromLatin1(QUrl::toPercentEncoding(t))); }},
        {QStringLiteral("unurl"), QStringLiteral("URL Decode"), {QStringLiteral("url decode")},
         [](const QString &t) { return some(QUrl::fromPercentEncoding(t.toUtf8())); }},
        {QStringLiteral("html"), QStringLiteral("HTML Escape"), {QStringLiteral("html escape")}, [](const QString &t) { return some(t.toHtmlEscaped()); }},
        {QStringLiteral("unhtml"), QStringLiteral("HTML Unescape"), {QStringLiteral("html unescape"), QStringLiteral("strip tags")},
         [](const QString &t) { return some(QTextDocumentFragment::fromHtml(t).toPlainText()); }},
        {QStringLiteral("json"), QStringLiteral("JSON Pretty-Print"), {QStringLiteral("json"), QStringLiteral("pretty"), QStringLiteral("format json")},
         [](const QString &t) -> std::optional<QString> {
             const QJsonDocument doc = QJsonDocument::fromJson(t.toUtf8());
             if (doc.isNull())
                 return std::nullopt;
             return QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed();
         }},
        {QStringLiteral("minify"), QStringLiteral("JSON Minify"), {QStringLiteral("minify"), QStringLiteral("compact json")},
         [](const QString &t) -> std::optional<QString> {
             const QJsonDocument doc = QJsonDocument::fromJson(t.toUtf8());
             if (doc.isNull())
                 return std::nullopt;
             return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
         }},
    };
    return list;
}

QString counts(const QString &text)
{
    const qsizetype w = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
    const qsizetype l = text.isEmpty() ? 0 : text.count(u'\n') + 1;
    return QObject::tr("%1 words · %2 characters · %3 lines").arg(w).arg(text.size()).arg(l);
}

std::optional<Generated> generate(const QString &raw)
{
    const QString q = raw.simplified();
    const QString lower = q.toLower();
    auto *random = QRandomGenerator::system(); // a secure source, for passwords

    if (lower == QLatin1String("uuid") || lower == QLatin1String("guid")) {
        const QString v = QUuid::createUuid().toString(QUuid::WithoutBraces);
        return Generated{v, QObject::tr("A random UUID (version 4)"), v};
    }
    static const QRegularExpression password(QStringLiteral("^(?:password|pass|pw)(?: (\\d{1,3}))?$"));
    if (const auto m = password.match(lower); m.hasMatch()) {
        const int length = std::clamp(m.captured(1).isEmpty() ? 20 : m.captured(1).toInt(), 8, 128);
        static const QString letters = QStringLiteral("abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789");
        static const QString symbols = QStringLiteral("!@#$%^&*-_=+?");
        QString v;
        for (int i = 0; i < length; ++i) {
            const QString &set = i % 5 == 4 ? symbols : letters; // about one symbol in five
            v += set.at(random->bounded(int(set.size())));
        }
        std::shuffle(v.begin(), v.end(), *random);
        return Generated{v, QObject::tr("A random %1-character password").arg(length), v};
    }
    static const QRegularExpression pin(QStringLiteral("^pin(?: (\\d{1,2}))?$"));
    if (const auto m = pin.match(lower); m.hasMatch()) {
        const int length = std::clamp(m.captured(1).isEmpty() ? 6 : m.captured(1).toInt(), 3, 20);
        QString v;
        for (int i = 0; i < length; ++i)
            v += QChar(u'0' + random->bounded(10));
        return Generated{v, QObject::tr("A random %1-digit PIN").arg(length), v};
    }
    static const QRegularExpression range(QStringLiteral("^(?:random|rand|dice)(?: (-?\\d+) ?(?:-|to|\\.\\.) ?(-?\\d+))?$"));
    if (const auto m = range.match(lower); m.hasMatch()) {
        qint64 lo = 1, hi = lower.startsWith(QLatin1String("dice")) ? 6 : 100;
        if (!m.captured(1).isEmpty()) {
            lo = m.captured(1).toLongLong();
            hi = m.captured(2).toLongLong();
            if (lo > hi)
                std::swap(lo, hi);
        }
        const QString v = QString::number(lo + qint64(random->bounded(quint64(hi - lo + 1))));
        return Generated{v, QObject::tr("A random number from %1 to %2").arg(lo).arg(hi), v};
    }
    static const QRegularExpression lorem(QStringLiteral("^lorem(?: ipsum)?(?: (\\d{1,2}))?$"));
    if (const auto m = lorem.match(lower); m.hasMatch()) {
        const int paragraphs = std::clamp(m.captured(1).isEmpty() ? 1 : m.captured(1).toInt(), 1, 20);
        QStringList v;
        for (int i = 0; i < paragraphs; ++i)
            v << LoremText;
        return Generated{LoremText.left(60) + QStringLiteral("…"),
                         paragraphs == 1 ? QObject::tr("A paragraph of placeholder text") : QObject::tr("%1 paragraphs of placeholder text").arg(paragraphs),
                         v.join(QStringLiteral("\n\n"))};
    }
    static const QRegularExpression hash(QStringLiteral("^(md5|sha1|sha256|sha512) (.+)$"), QRegularExpression::CaseInsensitiveOption);
    if (const auto m = hash.match(q); m.hasMatch()) {
        const QString algorithm = m.captured(1).toLower();
        const auto method = algorithm == QLatin1String("md5")      ? QCryptographicHash::Md5
                          : algorithm == QLatin1String("sha1")     ? QCryptographicHash::Sha1
                          : algorithm == QLatin1String("sha256")   ? QCryptographicHash::Sha256
                                                                   : QCryptographicHash::Sha512;
        const QString v = QString::fromLatin1(QCryptographicHash::hash(m.captured(2).toUtf8(), method).toHex());
        return Generated{v, QObject::tr("%1 of “%2”").arg(algorithm.toUpper(), m.captured(2)), v};
    }
    if (lower == QLatin1String("timestamp") || lower == QLatin1String("unix time") || lower == QLatin1String("epoch")) {
        const QString v = QString::number(QDateTime::currentSecsSinceEpoch());
        return Generated{v, QObject::tr("Now, in seconds since 1970 (Unix time)"), v};
    }
    // A 10-digit (seconds) or 13-digit (milliseconds) number: a Unix time.
    static const QRegularExpression unixTime(QStringLiteral("^(\\d{10}|\\d{13})$"));
    if (unixTime.match(lower).hasMatch()) {
        const qint64 n = lower.toLongLong();
        const QDateTime t = lower.size() == 13 ? QDateTime::fromMSecsSinceEpoch(n) : QDateTime::fromSecsSinceEpoch(n);
        const QString v = QLocale().toString(t, QLocale::LongFormat);
        return Generated{v, QObject::tr("Unix time %1 (%2 UTC)").arg(lower, t.toUTC().toString(Qt::ISODate)), v};
    }
    return std::nullopt;
}

} // namespace Transforms
