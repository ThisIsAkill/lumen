#include "arguments.h"

#include <QRegularExpression>

namespace
{

// {argument}, {argument name="x" default="y"} (single quotes work too) and
// {query}. Attribute values may not contain their own quote character.
const QRegularExpression &placeholderPattern()
{
    static const QRegularExpression pattern(QStringLiteral(R"re(\{(?:query|argument((?:\s+[A-Za-z]+\s*=\s*(?:"[^"]*"|'[^']*'))*)\s*)\})re"));
    return pattern;
}

QHash<QString, QString> attributes(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral(R"re(([A-Za-z]+)\s*=\s*(?:"([^"]*)"|'([^']*)'))re"));
    QHash<QString, QString> out;
    for (auto it = pattern.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        out.insert(m.captured(1).toLower(), m.hasCaptured(2) ? m.captured(2) : m.captured(3));
    }
    return out;
}

Argument parse(const QRegularExpressionMatch &match)
{
    Argument a;
    if (!match.captured(0).startsWith(QLatin1String("{argument"))) {
        a.name = QStringLiteral("query");
        return a;
    }
    const QHash<QString, QString> attrs = attributes(match.captured(1));
    a.name = attrs.value(QStringLiteral("name"), QStringLiteral("argument"));
    a.placeholder = attrs.value(QStringLiteral("placeholder"));
    a.defaultValue = attrs.value(QStringLiteral("default"));
    a.optional = attrs.contains(QStringLiteral("default"));
    if (const QString options = attrs.value(QStringLiteral("options")); !options.isEmpty()) {
        for (const QString &o : options.split(u','))
            a.options << o.trimmed();
    }
    return a;
}

} // namespace

QList<Argument> Arguments::fromTemplate(const QString &text)
{
    QList<Argument> out;
    for (auto it = placeholderPattern().globalMatch(text); it.hasNext();) {
        const Argument a = parse(it.next());
        if (std::none_of(out.cbegin(), out.cend(), [&](const Argument &b) { return b.name == a.name; }))
            out << a;
    }
    return out;
}

QString Arguments::fill(const QString &text, const QHash<QString, QString> &values, const std::function<QString(const QString &)> &quote)
{
    QString out;
    qsizetype last = 0;
    for (auto it = placeholderPattern().globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        const Argument a = parse(m);
        const QString value = values.value(a.name);
        out += text.mid(last, m.capturedStart() - last) + quote(value.isEmpty() ? a.defaultValue : value);
        last = m.capturedEnd();
    }
    return out + text.mid(last);
}

QHash<QString, QString> Arguments::byName(const QList<Argument> &arguments, const QStringList &values)
{
    QHash<QString, QString> out;
    for (int i = 0; i < arguments.size(); ++i)
        out.insert(arguments.at(i).name, values.value(i));
    return out;
}
