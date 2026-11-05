#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

// A value a command asks for before it runs, typed into a field in the
// search row (see Launcher::askArguments). Commands, quick links, snippets,
// Raycast script commands and extensions all describe theirs this way.
struct Argument {
    QString name;        // how the command refers to it
    QString placeholder; // shown in the empty field; the name when empty
    QString defaultValue;
    bool optional = false;
    bool secret = false; // typed like a password
    // A choice instead of free text: what is shown, and what is passed (the
    // same when values is empty).
    QStringList options;
    QStringList values;

    QString label() const { return placeholder.isEmpty() ? name : placeholder; }
    // The value passed for option `index`.
    QString valueAt(int index) const { return values.value(index, options.value(index)); }
};

namespace Arguments
{

// The arguments in a template, in order of first use. A template asks for
// one with {argument name="to"}, which may also say placeholder="…",
// default="…" and options="a,b,c"; a name used twice is asked once.
// {query} is an argument named "query", as in Lumen's older commands.
QList<Argument> fromTemplate(const QString &text);

// Replaces every argument in a template with its value (or default), passed
// through `quote` (shell quoting, URL encoding).
QString fill(const QString &text, const QHash<QString, QString> &values,
             const std::function<QString(const QString &)> &quote = [](const QString &v) { return v; });

// Values in the order of `arguments`, keyed by name.
QHash<QString, QString> byName(const QList<Argument> &arguments, const QStringList &values);

} // namespace Arguments
