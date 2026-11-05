#pragma once

#include <QList>
#include <QString>

#include <functional>
#include <optional>

// Text transformations (case, encodings, JSON, lines) and generators (UUIDs,
// passwords, hashes, timestamps, lorem ipsum). Pure functions: tested
// without a launcher.
namespace Transforms
{

struct Transform {
    QString id;       // "upper"
    QString name;     // "UPPERCASE"
    QStringList keywords;
    // The result, or nullopt when it does not apply ("base64 decode" of text
    // that is not Base64).
    std::function<std::optional<QString>(const QString &)> apply;
};

const QList<Transform> &all();

// Counts for the "count" answer: "12 words · 80 characters · 3 lines".
QString counts(const QString &text);

struct Generated {
    QString title;    // shown
    QString subtitle;
    QString value;    // copied or pasted
};
// "uuid", "password", "password 24", "pin 6", "random", "random 1-100",
// "lorem", "lorem 3", "sha256 text", "md5 text", "timestamp", "1793817403".
std::optional<Generated> generate(const QString &query);

} // namespace Transforms
