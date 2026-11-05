#include "fuzzy.h"

#include <QList>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cstdlib>

namespace
{

bool wordsMatch(const QStringList &queryWords, const QString &text)
{
    // Word starts: after a space, dash, underscore, dot or slash, or at a
    // lower→upper case change ("QuickLook").
    QStringList textWords;
    QString current;
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        const bool separator = c.isSpace() || c == u'-' || c == u'_' || c == u'.' || c == u'/';
        const bool camel = i > 0 && c.isUpper() && text.at(i - 1).isLower();
        if ((separator || camel) && !current.isEmpty()) {
            textWords << current.toLower();
            current.clear();
        }
        if (!separator)
            current += c;
    }
    if (!current.isEmpty())
        textWords << current.toLower();

    qsizetype next = 0;
    for (const QString &q : queryWords) {
        while (next < textWords.size() && !textWords.at(next).startsWith(q))
            ++next;
        if (next == textWords.size())
            return false;
        ++next;
    }
    return true;
}

bool inOrder(const QString &query, const QString &text)
{
    qsizetype pos = 0;
    for (const QChar c : query) {
        if (c.isSpace())
            continue;
        pos = text.indexOf(c, pos, Qt::CaseInsensitive);
        if (pos < 0)
            return false;
        ++pos;
    }
    return true;
}

// Edit distance with adjacent swaps (optimal string alignment), giving up
// once it is certain to exceed `limit`.
int typoDistance(const QString &a, const QString &b, int limit)
{
    if (std::abs(a.size() - b.size()) > limit)
        return limit + 1;
    QList<int> before(b.size() + 1), previous(b.size() + 1), current(b.size() + 1);
    for (int j = 0; j <= b.size(); ++j)
        previous[j] = j;
    for (int i = 1; i <= a.size(); ++i) {
        current[0] = i;
        int rowBest = current[0];
        for (int j = 1; j <= b.size(); ++j) {
            const int cost = a.at(i - 1) == b.at(j - 1) ? 0 : 1;
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, previous[j - 1] + cost});
            if (i > 1 && j > 1 && a.at(i - 1) == b.at(j - 2) && a.at(i - 2) == b.at(j - 1))
                current[j] = std::min(current[j], before[j - 2] + 1);
            rowBest = std::min(rowBest, current[j]);
        }
        if (rowBest > limit)
            return limit + 1;
        std::swap(before, previous);
        std::swap(previous, current);
    }
    return previous[b.size()];
}

// Whether every query word is a slightly misspelt word (or word start) of
// the text, in order.
bool typoMatch(const QString &query, const QString &text)
{
    const QStringList queryWords = query.toLower().split(u' ', Qt::SkipEmptyParts);
    static const QRegularExpression separators(QStringLiteral("[\\s\\-_./]+"));
    const QStringList textWords = text.toLower().split(separators, Qt::SkipEmptyParts);
    qsizetype next = 0;
    for (const QString &q : queryWords) {
        if (q.size() < 4)
            return false; // short words have too many neighbours
        const int limit = q.size() >= 8 ? 2 : 1;
        bool found = false;
        for (; next < textWords.size() && !found; ++next) {
            const QString &t = textWords.at(next);
            // Compare with the whole word and with its start ("firfo" → "firefox").
            found = typoDistance(q, t, limit) <= limit || typoDistance(q, t.left(q.size()), limit) <= limit;
        }
        if (!found)
            return false;
    }
    return !queryWords.isEmpty();
}

} // namespace

qreal fuzzyScore(const QString &rawQuery, const QString &text)
{
    const QString query = rawQuery.trimmed();
    if (query.isEmpty() || text.isEmpty())
        return 0;
    const qreal lengthBonus = 5.0 / (1 + text.size() / 8.0); // at most +5

    if (text.compare(query, Qt::CaseInsensitive) == 0)
        return 100;
    if (text.startsWith(query, Qt::CaseInsensitive))
        return 80 + lengthBonus;
    if (wordsMatch(query.toLower().split(u' ', Qt::SkipEmptyParts), text))
        return 60 + lengthBonus;
    if (text.contains(query, Qt::CaseInsensitive))
        return 40 + lengthBonus;
    // Letters in order only count for short names; in long texts almost any
    // query would match somewhere.
    if (query.size() >= 2 && text.size() <= 32 && inOrder(query, text))
        return 20 + lengthBonus;
    if (typoMatch(query, text))
        return 10 + lengthBonus;
    return 0;
}
