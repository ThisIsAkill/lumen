#pragma once

#include "library.h"
#include "source.h"

class Library;

// Lumen's own root results: its commands (clipboard history, file search,
// settings, ...), window layouts, snippets and quick links.
class BuiltinSource : public Source
{
    Q_OBJECT
public:
    BuiltinSource(Launcher *launcher, Library *library);

    void query(const QString &text) override;
    QList<Item> fallbackItems(const QString &text) const override;

    // Everything, for the start screen (empty query).
    QList<Item> allCommands() const;

    // Snippets matching text (all of them for an empty text).
    QList<Item> snippetItems(const QString &text) const;

private:
    QList<Item> commands() const;
    QList<Item> quickLinkItems(const QString &text) const;
    Action askAction(const Library::QuickLink &link, const QStringList &initial) const;
    static QString argumentNames(const QList<Argument> &arguments);

    Library *m_library;
};

// The "Search Snippets" view.
class SnippetsSource : public Source
{
    Q_OBJECT
public:
    SnippetsSource(Launcher *launcher, BuiltinSource *builtin)
        : Source(launcher)
        , m_builtin(builtin)
    {
    }
    void query(const QString &text) override { Q_EMIT resultsReady(m_builtin->snippetItems(text.trimmed())); }
    QString viewTitle() const override { return tr("Snippets"); }
    QString viewPlaceholder() const override { return tr("Search snippets…"); }

private:
    BuiltinSource *m_builtin;
};
