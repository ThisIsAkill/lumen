#pragma once

#include "clipboardhistory.h"
#include "source.h"

#include <QKeySequence>

class QAction;

// Copy several things, then paste them one after another: while the stack
// is on, every copy joins it, and Ctrl+Alt+V (a global shortcut that exists
// only while the stack does) pastes the oldest into the focused app. The
// stack ends by itself once it is empty.
class PasteStack : public QObject
{
    Q_OBJECT
public:
    PasteStack(Launcher *launcher, ClipboardHistory *history);
    ~PasteStack() override;

    bool isActive() const { return m_active; }
    void start();
    void stop();
    QList<ClipboardHistory::Entry> items() const { return m_items; }

    void add(const ClipboardHistory::Entry &entry);
    void remove(int index);
    void pasteNext(); // the oldest, into the focused app
    void paste(int index, bool now);
    void pasteAll(); // the text items, one per line

    static QKeySequence nextKey() { return QKeySequence(Qt::CTRL | Qt::ALT | Qt::Key_V); }

Q_SIGNALS:
    void changed();

private:
    bool isOurOwn(const ClipboardHistory::Entry &entry) const;

    Launcher *m_launcher;
    ClipboardHistory *m_history;
    QAction *m_next;
    bool m_active = false;
    QList<ClipboardHistory::Entry> m_items;
    ClipboardHistory::Entry m_pasted; // what we just put on the clipboard: not a new copy
};

// The "Paste Stack" view: what is waiting, oldest first.
class PasteStackSource : public Source
{
    Q_OBJECT
public:
    PasteStackSource(Launcher *launcher, PasteStack *stack);

    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Paste Stack"); }
    QString viewPlaceholder() const override { return tr("Copy things to add them…"); }
    bool hasPreviewPane() const override { return true; }

    // A start-screen row while the stack is on.
    QList<Item> startItems() const;

private:
    PasteStack *m_stack;
    QString m_query;
};
