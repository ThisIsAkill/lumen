#pragma once

#include <QString>

// "1 item" or "3 items": `one` or `many` with the count as %1. (Qt's own
// plurals, tr("%n item(s)"), need translation files, which Lumen does not
// ship; without them the "(s)" would show.)
inline QString plural(qint64 count, const QString &one, const QString &many)
{
    return (count == 1 ? one : many).arg(count);
}
