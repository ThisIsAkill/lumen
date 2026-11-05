#pragma once

#include "library.h"

#include <QDialog>

class ClipboardHistory;
class KKeySequenceWidget;
class Launcher;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QTableWidget;

// Lumen's settings window: the launcher shortcut, clipboard history options,
// and editors for snippets and quick links. Changes apply immediately.
class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SettingsDialog(Launcher *launcher);

    // tab is a Launcher::SettingsTab; addNew starts a new snippet or link.
    void showTab(int tab, bool addNew);

    // Applies the saved clipboard options at start-up.
    static void applySavedSettings(ClipboardHistory *history);

private:
    QWidget *generalPage();
    QWidget *snippetsPage();
    QWidget *quickLinksPage();
    QWidget *commandsPage();
    QWidget *calendarPage();
    QWidget *focusPage();
    void saveCommands();
    QComboBox *outputBox(int output);
    void loadSnippet(int row);
    void saveSnippets();
    void saveQuickLinks();
    void fillQuickLinks();

    Launcher *m_launcher;
    Library *m_library;
    QTabWidget *m_tabs;
    KKeySequenceWidget *m_shortcut = nullptr;
    QCheckBox *m_clipboardEnabled = nullptr;
    QSpinBox *m_clipboardLimit = nullptr;

    QList<Library::Snippet> m_snippets;
    QListWidget *m_snippetList = nullptr;
    QLineEdit *m_snippetName = nullptr;
    QLineEdit *m_snippetKeyword = nullptr;
    QPlainTextEdit *m_snippetText = nullptr;
    bool m_loadingSnippet = false;

    QTableWidget *m_links = nullptr;
    QTableWidget *m_commands = nullptr;
    bool m_loadingLinks = false;
};
