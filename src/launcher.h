#pragma once

#include "arguments.h"
#include "item.h"
#include "usage.h"

#include <KService>

#include <motion.h>

#include <QHash>
#include <QDateTime>
#include <QPointer>
#include <QSet>
#include <QWidget>

#include <functional>
#include <optional>

class BuiltinSource;
class CalendarSource;
class SystemSource;
class Timekeeper;
class TimeSource;
class ClipboardHistory;
class Hud;
class KeyHints;
class Expansion;
class AppHotkeys;
class ArgumentBar;
class Library;
class Panel;
class NotesWindow;
class Paster;
class PreviewPane;
class QAction;
class QTimer;
class QLabel;
class QLineEdit;
class ResultList;
class ResultModel;
class SearchGlyph;
class SettingsDialog;
class Source;
class StatusLine;
class ViewChip;

// The launcher window: a search field over a result list, shown and hidden
// with a global shortcut (Meta+Space by default).
//
// The main search merges results from KRunner and Lumen's own commands.
// Commands can open a view (clipboard history, files, snippets) that takes
// over the list until Esc or Backspace in an empty field. Ctrl+K lists the
// selected result's actions.
//
// The card springs open and its picture shrinks away when it closes; views
// slide in and out, the selection glides, new results settle in (see
// motion.h and ResultList). The state changes at once in every case: only
// the picture takes its time.
class Launcher : public QWidget
{
    Q_OBJECT
public:
    enum View { RootView, ClipboardView, FilesView, SnippetsView, EmojiView, WindowsView, TransformView, ProcessesView, DownloadsView, PasteStackView, MenusView, TabsView, HistoryView, CalculationsView, ThemeView, PackagesView, AppsView, AiChatView };
    enum SettingsTab { GeneralTab, SnippetsTab, QuickLinksTab, CommandsTab, CalendarTab, FocusTab };

    Launcher();
    ~Launcher() override;

    void toggle();
    void showWithQuery(const QString &query);
    void hideLauncher();

    void openView(View view, const QString &query = {});
    // Opens a view over the current one (an extension's list, a nested
    // list); Esc or Backspace in the empty field go back to it. A source
    // without a parent becomes the launcher's and is deleted once left.
    void pushView(Source *source, const QString &query = {});
    // Shows the launcher directly in a view ("clipboard", "files", "snippets").
    bool showView(const QString &name, const QString &query);
    void setQuery(const QString &query);
    Source *currentSource() const { return m_view; }

    // Helpers for actions.
    void copyText(const QString &text);
    void copyImage(const QString &pngPath);
    // Puts text or an image on the clipboard and pastes it into the app that
    // had focus before Lumen opened (when canPaste()).
    // cursorBack: then move the cursor back that many characters.
    void pasteText(const QString &text, int cursorBack = 0);
    void pasteImage(const QString &pngPath);
    // Pasting is turned on in the settings and works in this session.
    bool canPaste() const;
    // Replaces the `count` characters just typed in the focused app with
    // text (snippet expansion); the clipboard is restored afterwards.
    void replaceTypedText(int count, const QString &text, int cursorBack = 0);
    // Puts text on the clipboard and pastes it into the focused window
    // right away (Lumen is not open: a global shortcut).
    void pasteNow(const QString &text);
    void pasteImageNow(const QString &pngPath);
    // Replaces what is copied with its plain text (no formatting), and
    // pastes it into the app that was in use.
    void pastePlainText();

    // A snippet: its arguments are asked for, its placeholders filled in,
    // then it is pasted, copied, or replaces the `typed` characters of its
    // keyword typed in another app (snippet expansion).
    enum class SnippetUse { Paste, Copy, ReplaceTyped };
    void useSnippet(const QString &name, const QString &body, SnippetUse use, int typed = 0);
    // The nth most recent copied text (0: what is copied now).
    QString recentClipboardText(int n) const;
    class PasteStack *pasteStack() const { return m_pasteStack; }
    class FocusMode *focusMode() const { return m_focus; }
    Expansion *expansion() const { return m_expansion; }
    AppHotkeys *appHotkeys() const { return m_hotkeys; }
    void launchApp(const QString &storageId);
    void openUrl(const QUrl &url); // in the default browser or app

    // Uses the search field to ask for text (secret: hidden as typed, for
    // passwords), showing `shown` as what it is about; Enter answers, Esc
    // cancels.
    void ask(const QString &chip, const QString &placeholder, const Item &shown, bool secret,
             std::function<void(const QString &)> done);
    // Asks for a command's arguments in fields in the search row (Tab moves
    // between them, Enter runs, Esc cancels); `initial` pre-fills the first
    // fields. done gets one value per argument (the default when left empty).
    void askArguments(const Item &shown, const QList<Argument> &arguments, const QStringList &initial,
                      std::function<void(const QStringList &)> done);
    // Waits for a key combination (for setting a hotkey).
    void askForKeys(const QString &chip, const QString &placeholder, const Item &shown, std::function<void(const QKeySequence &)> done);
    void copyUrls(const QList<QUrl> &urls);
    // Shows or hides the preview of a file over the launcher.
    void quickLook(const QString &path);
    void openSettings(SettingsTab tab, bool addNew);
    void openNotes();
    NotesWindow *notes(); // created on first use
    void runAfterHide(std::function<void()> work);
    void notify(const QString &message); // a short message, also when hidden

    // Every command that can be a fallback, as it reads for “…”.
    QList<Item> fallbackChoices() const;

    QAction *toggleAction() const { return m_toggleAction; }
    ClipboardHistory *clipboardHistory() const { return m_clipboard; }
    Library *library() const { return m_library; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupWindow();
    void layoutCard();
    void animateIn();
    void revealList(int how); // the next results slide in (a ResultList::Reveal)
    void pressHint(const QString &keys); // the footer's key cap for a pressed shortcut
    void updatePane(); // shows the side preview in views that have one
    void restyle(); // palette-dependent style sheets (scrollbar, labels)
    void setupShortcut();
    void textChanged(const QString &text);
    void sourceResults(Source *source, const QList<Item> &items);
    void setView(Source *source, const QString &query);
    void connectSource(Source *source);
    void dropViews(); // empties the view stack, deleting the views it owns
    bool goBack();    // to the view below, or the main search; false when there
    void goForward(); // to the view just left with goBack()
    QList<Item> fallbackItems(const QString &text);
    void showRootResults();
    void showItems(const QList<Item> &items);
    void showActions(bool show);
    void runAction(const Item &item, const Action &action);
    void runSelected(int actionIndex);
    const Item *selectedItem() const;
    void moveSelection(int delta);
    void selectionChanged();
    void updateFooter();
    void setClipboardText(const QString &text);
    void setClipboardImage(const QString &pngPath);
    bool handleKey(QKeyEvent *event);

    Usage m_usage;
    Library *m_library;
    ClipboardHistory *m_clipboard;
    BuiltinSource *m_builtin;
    SystemSource *m_system;
    Timekeeper *m_timekeeper;
    TimeSource *m_time;
    CalendarSource *m_calendar;
    QList<Source *> m_rootSources;
    QHash<View, Source *> m_views;
    Source *m_view = nullptr; // the open view, or nullptr for the main search
    QHash<Source *, QList<Item>> m_rootResults;
    QList<std::pair<Source *, QString>> m_viewStack; // views below the open one, with their search text
    QSet<Source *> m_connected; // sources whose results reach the list
    QSet<Source *> m_ownedViews; // pushed views the launcher deletes
    QSet<QString> m_fallbackIds; // results that can be a fallback command
    QList<std::pair<Source *, QString>> m_forward; // views left with goBack(), for goForward()
    // Where the launcher was when it closed, to reopen there for a while
    // (lumenrc [General] ResumeSeconds; not after running something).
    QDateTime m_closedAt;
    Source *m_closedView = nullptr;
    QString m_closedQuery;

    // Favourites and aliases (see Library::Favourite and Library::Alias).
    QList<Action> personalActions(const Item &item) const;
    KService::Ptr serviceFor(const Item &item) const;
    QList<Item> favouriteItems();
    void toggleFavourite(const Item &item);
    void runFavourite(const QString &id, const QString &title);
    void startAliasInput(const Item &item);

    bool m_actionsShown = false;
    // Asking the user for something in the search field (see ask()).
    struct Prompt {
        std::function<void(const QString &)> onText;      // Enter
        std::function<void(const QKeySequence &)> onKeys; // a key combination, when capturing keys
        std::function<void(const QStringList &)> onArguments; // the argument fields, when asking for arguments
    };
    bool handleArgumentKey(QKeyEvent *event);
    void startPrompt(const QString &chip, const QString &placeholder, const Item &shown, bool secret, const Prompt &prompt);
    void endPrompt();
    std::optional<Prompt> m_prompt;
    QString m_aliasId;              // result brought up by the alias just typed
    QString m_pendingRunId;         // favourite being looked up to run
    QTimer *m_pendingTimeout = nullptr;
    bool m_userMoved = false; // keep the user's selection while results refine
    QString m_reselectId;     // result to select again once it reappears
    Item m_actionsItem;      // the result whose actions are listed
    QString m_savedQuery;    // the search text while actions are listed

    QWidget *m_card;
    SearchGlyph *m_glyph;
    ViewChip *m_chip;
    ArgumentBar *m_args;
    QLineEdit *m_search;
    ResultList *m_list;
    ResultModel *m_model;
    StatusLine *m_status;
    KeyHints *m_hints;
    QAction *m_toggleAction;
    Hud *m_hud;
    Panel *m_preview = nullptr;
    Paster *m_paster;
    class PasteStack *m_pasteStack;
    class FocusMode *m_focus;
    class KeepAwake *m_awake;
    class AiChat *m_ai;
    Expansion *m_expansion;
    AppHotkeys *m_hotkeys;
    PreviewPane *m_pane;
    QPointer<SettingsDialog> m_settings;
    NotesWindow *m_notes = nullptr;
    bool m_overlay = false; // full-screen transparent layer surface (Wayland)

    Tween m_opening;     // 0 → 1 while the card springs open
    Motion::Pose m_pose; // how the card is drawn meanwhile
    Tween m_cardWidth;   // the card's width, eased when the preview pane comes and goes
    Tween m_glow;        // the outline around the field while it asks something
};
