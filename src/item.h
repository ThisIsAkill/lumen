#pragma once

#include <QIcon>
#include <QKeySequence>
#include <QList>
#include <QString>

#include <functional>

// Something the user can do with a result. The first action of an item is
// its primary one (Enter); the rest appear in the actions list (Ctrl+K).
struct Action {
    QString text;
    QString iconName;
    QKeySequence shortcut;
    std::function<void()> run;
    bool keepOpen = false; // the launcher stays open after running it
    bool asksArguments = false; // it opens the argument fields (Tab does too)
};

// One row in the result list.
struct Item {
    QString id;       // stable identifier, used for usage ranking
    QString title;
    QString subtitle;
    QString kind;     // shown on the right: "Application", "Command", ...
    bool keyKind = false; // the kind is a keyboard shortcut, drawn as key caps
    QIcon icon;
    qreal score = 0;  // higher is better; see fuzzy.h for the scale
    qreal fraction = -1; // 0–1 draws a small bar under the text (memory used, a timer's progress); -1 for none
    QString filePath; // set for local files, enables Quick Look
    QString previewText; // shown in the preview pane when there is no file
    bool markdown = false; // previewText is Markdown, shown rendered
    bool monospace = false; // previewText is a program's output, shown in a fixed-pitch font
    QString previewImage; // an image shown in the preview pane instead (window thumbnails)
    QList<Action> actions;
};
