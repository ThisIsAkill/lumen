#include "settingsdialog.h"
#include "calendarsource.h"
#include "clipboardhistory.h"
#include "expansion.h"
#include "focusmode.h"
#include "launcher.h"
#include "scriptcommands.h"
#include "theme.h"
#include "scriptsource.h"
#include "weathersource.h"

#include <chrome.h>

#include <KConfigGroup>
#include <KGlobalAccel>
#include <KApplicationTrader>
#include <KKeySequenceWidget>
#include <KSharedConfig>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <KIO/OpenUrlJob>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace
{

KConfigGroup clipboardConfig()
{
    return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Clipboard"));
}

constexpr int PageMargin = 20;  // page edge to its controls
constexpr int SectionGap = 10;  // above a section heading

// Explanatory text under a setting: smaller, in the muted text colour.
QLabel *note(const QString &text)
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setFont(Chrome::scaledFont(label->font(), 0.92));
    label->setStyleSheet(QStringLiteral("color: %1").arg(Chrome::mutedText(label->palette()).name(QColor::HexArgb)));
    label->setContentsMargins(0, 0, 0, 2);
    return label;
}

// A section heading spanning the form, with air above it.
QLabel *heading(const QString &text, bool first = false)
{
    auto *label = new QLabel(text);
    label->setFont(Chrome::scaledFont(label->font(), 1.0, QFont::DemiBold));
    label->setContentsMargins(0, first ? 0 : SectionGap, 0, 2);
    return label;
}

// A page that scrolls when the window is shorter than its sections: the
// area is frameless and transparent, so it is not seen as a widget.
QScrollArea *scrolling(QWidget *page)
{
    auto *area = new QScrollArea;
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);
    return area;
}

// A form with the dialog's spacing: labels right-aligned beside their
// controls, rows breathing a little more than the style's default.
QFormLayout *form(QWidget *page)
{
    auto *layout = new QFormLayout(page);
    layout->setContentsMargins(PageMargin, PageMargin - 4, PageMargin, PageMargin);
    layout->setHorizontalSpacing(16);
    layout->setVerticalSpacing(6);
    layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Fields take the full width whatever the platform theme prefers, so
    // wrapped notes get their height instead of being clipped.
    layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    layout->setRowWrapPolicy(QFormLayout::DontWrapRows);
    return layout;
}

// Add/Remove buttons under a list or table, with any extra buttons at
// the right.
QHBoxLayout *buttonRow(QPushButton *add, QPushButton *remove, QPushButton *extra = nullptr)
{
    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch();
    if (extra)
        row->addWidget(extra);
    return row;
}

// Tables of the library's entries: no grid, alternating rows, a tidy header.
void styleTable(QTableWidget *table, int stretchColumn)
{
    table->horizontalHeader()->setSectionResizeMode(stretchColumn, QHeaderView::Stretch);
    table->horizontalHeader()->setHighlightSections(false);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(30);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->setShowGrid(false);
    table->setColumnWidth(0, 170);
    table->setColumnWidth(1, 110);
}

// A label that elides a long path in the middle instead of widening the
// dialog, with the home folder shortened to "~".
class PathLabel : public QLabel
{
public:
    using QLabel::QLabel;

    void setPath(const QString &path)
    {
        m_path = path;
        if (m_path.startsWith(QDir::homePath()))
            m_path.replace(0, QDir::homePath().size(), QStringLiteral("~"));
        setToolTip(path);
        update();
    }

    QSize minimumSizeHint() const override { return QSize(80, QLabel::minimumSizeHint().height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(palette().color(QPalette::WindowText));
        p.drawText(contentsRect(), Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(m_path, Qt::ElideMiddle, contentsRect().width()));
    }

private:
    QString m_path;
};

} // namespace

SettingsDialog::SettingsDialog(Launcher *launcher)
    : QDialog(nullptr)
    , m_launcher(launcher)
    , m_library(launcher->library())
    , m_tabs(new QTabWidget(this))
{
    setWindowTitle(tr("Lumen Settings"));
    setWindowIcon(QIcon::fromTheme(QStringLiteral("search")));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(760, 560);
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(scrolling(generalPage()), tr("General"));
    m_tabs->addTab(snippetsPage(), tr("Snippets"));
    m_tabs->addTab(quickLinksPage(), tr("Quick Links"));
    m_tabs->addTab(scrolling(commandsPage()), tr("Commands"));
    m_tabs->addTab(calendarPage(), tr("Calendar && Weather"));
    m_tabs->addTab(focusPage(), tr("Focus"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_tabs);
}

void SettingsDialog::applySavedSettings(ClipboardHistory *history)
{
    const KConfigGroup group = clipboardConfig();
    history->setEnabled(group.readEntry("Enabled", true));
    history->setLimit(group.readEntry("Limit", 200));
}

void SettingsDialog::showTab(int tab, bool addNew)
{
    m_tabs->setCurrentIndex(tab);
    show();
    raise();
    activateWindow();
    if (!addNew)
        return;
    if (tab == Launcher::SnippetsTab) {
        m_snippets << Library::Snippet{tr("New Snippet"), {}, {}};
        m_snippetList->addItem(m_snippets.last().name);
        m_snippetList->setCurrentRow(m_snippetList->count() - 1);
        saveSnippets();
        m_snippetName->setFocus();
        m_snippetName->selectAll();
    } else if (tab == Launcher::CommandsTab) {
        m_loadingLinks = true;
        const int row = m_commands->rowCount();
        m_commands->insertRow(row);
        m_commands->setItem(row, 0, new QTableWidgetItem(tr("New Command")));
        m_commands->setItem(row, 1, new QTableWidgetItem);
        m_commands->setItem(row, 2, new QTableWidgetItem(QStringLiteral("echo {query}")));
        m_commands->setCellWidget(row, 3, outputBox(Library::Command::Notify));
        m_loadingLinks = false;
        saveCommands();
        m_commands->setCurrentCell(row, 0);
        m_commands->editItem(m_commands->item(row, 0));
    } else if (tab == Launcher::QuickLinksTab) {
        m_loadingLinks = true;
        const int row = m_links->rowCount();
        m_links->insertRow(row);
        m_links->setItem(row, 0, new QTableWidgetItem(tr("New Link")));
        m_links->setItem(row, 1, new QTableWidgetItem);
        m_links->setItem(row, 2, new QTableWidgetItem(QStringLiteral("https://")));
        auto *app = new QComboBox;
        app->addItem(QIcon::fromTheme(QStringLiteral("internet-web-browser")), tr("Default"), QString());
        for (const KService::Ptr &b : KApplicationTrader::queryByMimeType(QStringLiteral("x-scheme-handler/https")))
            app->addItem(QIcon::fromTheme(b->icon()), b->name(), b->storageId());
        connect(app, &QComboBox::currentIndexChanged, this, [this] { saveQuickLinks(); });
        m_links->setCellWidget(row, 3, app);
        m_loadingLinks = false;
        saveQuickLinks();
        m_links->setCurrentCell(row, 0);
        m_links->editItem(m_links->item(row, 0));
    }
}

// --------------------------------------------------------------- general

QWidget *SettingsDialog::generalPage()
{
    auto *page = new QWidget;
    auto *form = ::form(page);

    form->addRow(heading(tr("Shortcut"), true));
    m_shortcut = new KKeySequenceWidget(page);
    m_shortcut->setCheckForConflictsAgainst(KKeySequenceWidget::GlobalShortcuts | KKeySequenceWidget::StandardShortcuts);
    m_shortcut->setKeySequence(KGlobalAccel::self()->shortcut(m_launcher->toggleAction()).value(0));
    connect(m_shortcut, &KKeySequenceWidget::keySequenceChanged, this, [this](const QKeySequence &seq) {
        KGlobalAccel::self()->setShortcut(m_launcher->toggleAction(), {seq}, KGlobalAccel::NoAutoloading);
    });
    form->addRow(tr("Open Lumen with:"), m_shortcut);
    form->addRow(QString(), note(tr("Also under System Settings → Keyboard → Shortcuts → Lumen.")));

    KConfigGroup general = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("General"));
    form->addRow(heading(tr("Appearance")));
    // The same swatches as the Lumen Theme view; they follow the choices.
    auto *theme = new QComboBox(page);
    auto *accent = new QComboBox(page);
    theme->setIconSize(QSize(20, 20));
    accent->setIconSize(QSize(20, 20));
    auto fillSwatches = [theme, accent] {
        for (int i = 0; i < theme->count(); ++i)
            theme->setItemIcon(i, Theme::themeSwatch(theme->itemData(i).toString(), accent->currentData().toString()));
        for (int i = 0; i < accent->count(); ++i)
            accent->setItemIcon(i, Theme::accentSwatch(accent->itemData(i).toString()));
    };
    for (const Theme::Choice &t : Theme::themes())
        theme->addItem(t.name, t.id);
    for (const Theme::Accent &a : Theme::accents())
        accent->addItem(a.name, a.id);
    theme->setCurrentIndex(std::max(0, theme->findData(Theme::currentTheme())));
    accent->setCurrentIndex(std::max(0, accent->findData(Theme::currentAccent())));
    fillSwatches();
    auto applyTheme = [theme, accent, fillSwatches] {
        Theme::apply(theme->currentData().toString(), accent->currentData().toString());
        fillSwatches();
    };
    connect(theme, &QComboBox::currentIndexChanged, this, applyTheme);
    connect(accent, &QComboBox::currentIndexChanged, this, applyTheme);
    form->addRow(tr("Theme:"), theme);
    form->addRow(tr("Accent colour:"), accent);
    form->addRow(QString(), note(tr("Lumen's own look, whatever the system colour scheme; “System” follows it. "
                                    "Type “theme” in Lumen to switch from there.")));

    auto *resume = new QComboBox(page);
    for (const auto &[seconds, label] : QList<std::pair<int, QString>>{{0, tr("Never")}, {30, tr("30 seconds")}, {90, tr("90 seconds")},
                                                                        {300, tr("5 minutes")}, {900, tr("15 minutes")}})
        resume->addItem(label, seconds);
    resume->setCurrentIndex(std::max(0, resume->findData(general.readEntry("ResumeSeconds", 90))));
    connect(resume, &QComboBox::currentIndexChanged, this, [resume] {
        KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("General"));
        g.writeEntry("ResumeSeconds", resume->currentData().toInt());
        g.sync();
    });
    form->addRow(tr("Reopen where I was within:"), resume);
    form->addRow(QString(), note(tr("Opened again that soon, Lumen comes back to the view and text you left (selected, so typing "
                                    "starts over). After running something it always starts afresh. Ctrl+[ and Ctrl+] go back and "
                                    "forward between views.")));

    // AI: the local Ollama server and model.
    form->addRow(heading(tr("AI")));
    KConfigGroup ai = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("AI"));
    auto *server = new QLineEdit(ai.readEntry("Server", QStringLiteral("http://localhost:11434")), page);
    auto *model = new QComboBox(page);
    model->setEditable(true);
    model->addItem(tr("First installed model"), QString());
    const QString chosen = ai.readEntry("Model");
    if (!chosen.isEmpty())
        model->addItem(chosen, chosen);
    model->setCurrentIndex(chosen.isEmpty() ? 0 : 1);
    // The installed models, from Ollama.
    auto *probe = new QProcess(page);
    connect(probe, &QProcess::finished, page, [probe, model, chosen] {
        probe->deleteLater();
        for (const QString &line : QString::fromUtf8(probe->readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts).mid(1)) {
            const QString name = line.section(u' ', 0, 0);
            if (!name.isEmpty() && model->findData(name) < 0)
                model->addItem(name, name);
        }
        model->setCurrentIndex(std::max(0, model->findData(chosen)));
    });
    probe->start(QStringLiteral("ollama"), {QStringLiteral("list")});
    auto saveAi = [server, model] {
        KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("AI"));
        g.writeEntry("Server", server->text().trimmed());
        const QString data = model->currentData().toString();
        g.writeEntry("Model", model->currentIndex() > 0 && !data.isEmpty() ? data : model->currentIndex() == 0 ? QString() : model->currentText());
        g.sync();
    };
    connect(server, &QLineEdit::editingFinished, this, saveAi);
    connect(model, &QComboBox::currentIndexChanged, this, saveAi);
    form->addRow(tr("Ollama server:"), server);
    form->addRow(tr("Model:"), model);
    form->addRow(QString(), note(tr("Type “ask …” (or “? …”) for a quick answer, or open AI Chat. Everything stays on this computer: "
                                    "the model runs in Ollama (ollama.com). Chats are kept in ~/.local/share/lumen/ai-chats.json.")));

    form->addRow(heading(tr("Pasting")));
    auto *paste = new QCheckBox(tr("Enter pastes clipboard items and snippets into the app you were using"), page);
    paste->setChecked(general.readEntry("PasteDirectly", true));
    connect(paste, &QCheckBox::toggled, this, [](bool on) {
        KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("General"));
        g.writeEntry("PasteDirectly", on);
        g.sync();
    });
    form->addRow(QString(), paste);
    form->addRow(QString(), note(tr("Ctrl+Enter copies without pasting. Terminals paste with Ctrl+Shift+V, so there use Ctrl+Enter and paste yourself.")));

    // Sync: the library (snippets, links, commands, favourites, aliases, notes)
    // in a folder that Syncthing, Nextcloud and the like keep in step.
    form->addRow(heading(tr("Sync")));
    auto *syncPath = new PathLabel(page);
    auto *chooseSync = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-sync")), tr("Choose Folder…"), page);
    auto *stopSync = new QPushButton(tr("Stop Syncing"), page);
    auto showSync = [this, syncPath, stopSync] {
        const bool syncing = m_library->folder() != Library::defaultFolder();
        syncPath->setPath(syncing ? m_library->folder() : tr("Not syncing (kept in %1)").arg(Library::defaultFolder()));
        stopSync->setVisible(syncing);
    };
    connect(chooseSync, &QPushButton::clicked, this, [this, showSync] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder to keep Lumen's library in"), QDir::homePath());
        if (!folder.isEmpty()) {
            m_library->setFolder(folder);
            showSync();
        }
    });
    connect(stopSync, &QPushButton::clicked, this, [this, showSync] {
        m_library->setFolder({});
        showSync();
    });
    showSync();
    auto *syncRow = new QHBoxLayout;
    syncRow->setSpacing(8);
    syncRow->addWidget(syncPath, 1);
    syncRow->addWidget(chooseSync);
    syncRow->addWidget(stopSync);
    form->addRow(tr("Library folder:"), syncRow);
    form->addRow(QString(), note(tr("Snippets, quick links, commands, favourites, aliases and notes are kept there, so a folder "
                                    "synced by Syncthing, Resilio or Nextcloud shares them between computers. "
                                    "Clipboard history stays on this computer.")));

    // Fallbacks: commands offered for any search, below the other results.
    form->addRow(heading(tr("Fallback Commands")));
    auto *fallbacks = new QListWidget(page);
    fallbacks->setMaximumHeight(130);
    auto *choices = new QComboBox(page);
    auto *addFallback = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"), page);
    auto *removeFallback = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), page);
    auto *up = new QPushButton(QIcon::fromTheme(QStringLiteral("go-up")), QString(), page);
    auto *down = new QPushButton(QIcon::fromTheme(QStringLiteral("go-down")), QString(), page);
    up->setToolTip(tr("Move Up"));
    down->setToolTip(tr("Move Down"));
    auto fillFallbacks = [this, fallbacks, choices] {
        QHash<QString, Item> byId;
        for (const Item &item : m_launcher->fallbackChoices())
            byId.insert(item.id, item);
        fallbacks->clear();
        for (const QString &id : m_library->fallbacks()) {
            auto *row = new QListWidgetItem(byId.value(id).icon, byId.contains(id) ? byId.value(id).title : id + tr(" (not available)"));
            row->setData(Qt::UserRole, id);
            fallbacks->addItem(row);
        }
        choices->clear();
        for (const Item &item : m_launcher->fallbackChoices())
            if (!m_library->fallbacks().contains(item.id))
                choices->addItem(item.icon, item.title, item.id);
    };
    auto move = [this, fallbacks, fillFallbacks](int delta) {
        QStringList ids = m_library->fallbacks();
        const int row = fallbacks->currentRow();
        if (row < 0 || row + delta < 0 || row + delta >= ids.size())
            return;
        ids.move(row, row + delta);
        m_library->setFallbacks(ids);
        fillFallbacks();
        fallbacks->setCurrentRow(row + delta);
    };
    connect(addFallback, &QPushButton::clicked, this, [this, choices, fillFallbacks] {
        if (choices->currentIndex() >= 0)
            m_library->setFallbacks(m_library->fallbacks() << choices->currentData().toString());
        fillFallbacks();
    });
    connect(removeFallback, &QPushButton::clicked, this, [this, fallbacks, fillFallbacks] {
        if (const QListWidgetItem *row = fallbacks->currentItem()) {
            QStringList ids = m_library->fallbacks();
            ids.removeAll(row->data(Qt::UserRole).toString());
            m_library->setFallbacks(ids);
            fillFallbacks();
        }
    });
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    fillFallbacks();
    auto *fallbackButtons = new QHBoxLayout;
    fallbackButtons->setSpacing(8);
    fallbackButtons->addWidget(choices, 1);
    fallbackButtons->addWidget(addFallback);
    fallbackButtons->addWidget(removeFallback);
    fallbackButtons->addWidget(up);
    fallbackButtons->addWidget(down);
    form->addRow(tr("Offer for every search:"), fallbacks);
    form->addRow(QString(), fallbackButtons);
    form->addRow(QString(), note(tr("Shown below the results as “Use … with”. Quick links, commands, script commands and "
                                    "extensions that take text can be added; Ctrl+K on one also offers “Use as Fallback”.")));

    form->addRow(heading(tr("Clipboard History")));
    m_clipboardEnabled = new QCheckBox(tr("Keep a history of copied text and images"), page);
    m_clipboardLimit = new QSpinBox(page);
    m_clipboardLimit->setRange(10, 2000);
    m_clipboardLimit->setSuffix(tr(" items"));
    const KConfigGroup group = clipboardConfig();
    m_clipboardEnabled->setChecked(group.readEntry("Enabled", true));
    m_clipboardLimit->setValue(group.readEntry("Limit", 200));
    auto save = [this] {
        KConfigGroup g = clipboardConfig();
        g.writeEntry("Enabled", m_clipboardEnabled->isChecked());
        g.writeEntry("Limit", m_clipboardLimit->value());
        g.sync();
        applySavedSettings(m_launcher->clipboardHistory());
    };
    connect(m_clipboardEnabled, &QCheckBox::toggled, this, save);
    connect(m_clipboardLimit, &QSpinBox::valueChanged, this, save);
    form->addRow(QString(), m_clipboardEnabled);
    form->addRow(tr("Remember up to:"), m_clipboardLimit);
    auto *recognize = new QCheckBox(tr("Find copied images by the text in them"), page);
    recognize->setChecked(group.readEntry("RecognizeText", true));
    connect(recognize, &QCheckBox::toggled, this, [this](bool on) {
        KConfigGroup g = clipboardConfig();
        g.writeEntry("RecognizeText", on);
        g.sync();
        m_launcher->clipboardHistory()->setRecognizer(ClipboardHistory::defaultRecognizer());
    });
    form->addRow(QString(), recognize);
    if (ClipboardHistory::defaultRecognizer().isEmpty() && recognize->isChecked())
        form->addRow(QString(), note(tr("Needs Tesseract with your language: sudo pacman -S tesseract tesseract-data-eng")));
    form->addRow(QString(), note(tr("Pinned items are always kept. Content that password managers mark as secret is never "
                                    "saved. The history is stored in ~/.local/share/lumen/clipboard.")));
    return page;
}

// -------------------------------------------------------------- snippets

QWidget *SettingsDialog::snippetsPage()
{
    auto *page = new QWidget;
    m_snippets = m_library->snippets();
    m_snippetList = new QListWidget(page);
    for (const Library::Snippet &s : std::as_const(m_snippets))
        m_snippetList->addItem(s.name);

    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"), page);
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), page);
    connect(add, &QPushButton::clicked, this, [this] { showTab(Launcher::SnippetsTab, true); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_snippetList->currentRow();
        if (row < 0)
            return;
        m_snippets.removeAt(row);
        delete m_snippetList->takeItem(row);
        saveSnippets();
    });

    m_snippetName = new QLineEdit(page);
    m_snippetKeyword = new QLineEdit(page);
    m_snippetKeyword->setPlaceholderText(tr("optional, e.g. addr"));
    m_snippetText = new QPlainTextEdit(page);
    m_snippetText->setPlaceholderText(tr("Text to copy"));
    auto changed = [this] {
        const int row = m_snippetList->currentRow();
        if (m_loadingSnippet || row < 0)
            return;
        m_snippets[row] = {m_snippetName->text(), m_snippetKeyword->text(), m_snippetText->toPlainText()};
        m_snippetList->item(row)->setText(m_snippetName->text());
        saveSnippets();
    };
    connect(m_snippetName, &QLineEdit::textEdited, this, changed);
    connect(m_snippetKeyword, &QLineEdit::textEdited, this, changed);
    connect(m_snippetText, &QPlainTextEdit::textChanged, this, changed);
    connect(m_snippetList, &QListWidget::currentRowChanged, this, &SettingsDialog::loadSnippet);

    m_snippetList->setAlternatingRowColors(true);
    auto *listColumn = new QVBoxLayout;
    listColumn->setSpacing(8);
    listColumn->addWidget(m_snippetList, 1);
    listColumn->addLayout(buttonRow(add, remove));
    auto *editor = new QFormLayout;
    editor->setContentsMargins(0, 0, 0, 0);
    editor->setHorizontalSpacing(12);
    editor->setVerticalSpacing(8);
    editor->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    editor->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    editor->setRowWrapPolicy(QFormLayout::DontWrapRows);
    editor->addRow(tr("Name:"), m_snippetName);
    editor->addRow(tr("Keyword:"), m_snippetKeyword);
    editor->addRow(tr("Text:"), m_snippetText);
    editor->addRow(QString(), note(tr("Placeholders fill in when the snippet is used: {date}, {time}, {datetime}, {day}, {uuid}, "
                                      "{clipboard} (or {clipboard offset=1}, the item before). Dates move with an offset and take a "
                                      "format: {date +3d}, {time -2h}, {date +1M format=\"yyyy-MM-dd\"}. {cursor} is where the "
                                      "cursor ends up, and {argument name=\"who\"} is asked for when the snippet is used.")));
    auto *columns = new QHBoxLayout;
    columns->setSpacing(18);
    columns->addLayout(listColumn, 2);
    columns->addLayout(editor, 3);

    // Expansion in any app (through lumen-expand-helper).
    KConfigGroup expansion = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Expansion"));
    auto *expand = new QCheckBox(tr("Expand keywords typed in any app, after"), page);
    expand->setChecked(expansion.readEntry("Enabled", true));
    auto *trigger = new QLineEdit(Expansion::trigger(), page);
    trigger->setMaxLength(3);
    trigger->setFixedWidth(trigger->fontMetrics().horizontalAdvance(QStringLiteral("MMMM")) + 16);
    trigger->setAlignment(Qt::AlignCenter);
    QLabel *status = note({});
    Expansion *client = m_launcher->expansion();
    auto showStatus = [client, status, trigger] {
        status->setText(client->isConnected()
                            ? tr("Type %1 and a snippet's keyword anywhere, e.g. %1addr.").arg(trigger->text())
                            : tr("Needs the expansion helper: run sudo helper/install-helper.sh in Lumen's source folder."));
    };
    auto save = [client, expand, trigger, showStatus] {
        KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Expansion"));
        g.writeEntry("Enabled", expand->isChecked());
        g.writeEntry("Trigger", trigger->text().isEmpty() ? QStringLiteral(";") : trigger->text());
        g.sync();
        client->sendKeywords();
        showStatus();
    };
    connect(expand, &QCheckBox::toggled, this, save);
    connect(trigger, &QLineEdit::editingFinished, this, save);
    connect(client, &Expansion::connectionChanged, this, showStatus);
    showStatus();
    auto *expandRow = new QHBoxLayout;
    expandRow->setSpacing(8);
    expandRow->addWidget(expand);
    expandRow->addWidget(trigger);
    expandRow->addStretch();

    // Two sections like the General tab: expansion above, the snippets
    // themselves below, each under its heading.
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(PageMargin, PageMargin - 4, PageMargin, PageMargin);
    layout->setSpacing(6);
    layout->addWidget(heading(tr("Expansion"), true));
    layout->addLayout(expandRow);
    layout->addWidget(status);
    layout->addWidget(heading(tr("Snippets")));
    layout->addLayout(columns, 1);

    if (!m_snippets.isEmpty())
        m_snippetList->setCurrentRow(0);
    else
        loadSnippet(-1);
    return page;
}

void SettingsDialog::loadSnippet(int row)
{
    m_loadingSnippet = true;
    const bool valid = row >= 0 && row < m_snippets.size();
    const Library::Snippet s = valid ? m_snippets.at(row) : Library::Snippet{};
    m_snippetName->setText(s.name);
    m_snippetKeyword->setText(s.keyword);
    m_snippetText->setPlainText(s.text);
    for (QWidget *w : {static_cast<QWidget *>(m_snippetName), static_cast<QWidget *>(m_snippetKeyword), static_cast<QWidget *>(m_snippetText)})
        w->setEnabled(valid);
    m_loadingSnippet = false;
}

void SettingsDialog::saveSnippets()
{
    m_library->setSnippets(m_snippets);
}

// ----------------------------------------------------------- quick links

QWidget *SettingsDialog::quickLinksPage()
{
    auto *page = new QWidget;
    m_links = new QTableWidget(0, 4, page);
    m_links->setHorizontalHeaderLabels({tr("Name"), tr("Keyword"), tr("URL or folder"), tr("Open with")});
    styleTable(m_links, 2);
    m_links->setColumnWidth(3, 170);
    fillQuickLinks();
    connect(m_links, &QTableWidget::itemChanged, this, [this] {
        if (!m_loadingLinks)
            saveQuickLinks();
    });

    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"), page);
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), page);
    auto *ready = new QPushButton(QIcon::fromTheme(QStringLiteral("bookmarks")), tr("Add Ready-Made Links"), page);
    connect(add, &QPushButton::clicked, this, [this] { showTab(Launcher::QuickLinksTab, true); });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_links->currentRow() >= 0) {
            m_links->removeRow(m_links->currentRow());
            saveQuickLinks();
        }
    });
    connect(ready, &QPushButton::clicked, this, [this] {
        m_library->addReadyMadeLinks();
        fillQuickLinks();
    });
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(PageMargin, PageMargin - 4, PageMargin, PageMargin);
    layout->setSpacing(8);
    layout->addWidget(m_links, 1);
    layout->addLayout(buttonRow(add, remove, ready));
    layout->addWidget(note(tr("Put {query} in a URL to search with it: with keyword “gh”, typing “gh qt layer shell” "
                              "searches GitHub. Links without {query}, and folders like ~/Downloads, open directly.")));
    return page;
}

// The table from the library; "Open with" offers the default and every
// installed browser (and other apps that open web pages).
void SettingsDialog::fillQuickLinks()
{
    m_loadingLinks = true;
    m_links->setRowCount(0);
    const KService::List browsers = KApplicationTrader::queryByMimeType(QStringLiteral("x-scheme-handler/https"));
    for (const Library::QuickLink &l : m_library->quickLinks()) {
        const int row = m_links->rowCount();
        m_links->insertRow(row);
        m_links->setItem(row, 0, new QTableWidgetItem(l.name));
        m_links->setItem(row, 1, new QTableWidgetItem(l.keyword));
        m_links->setItem(row, 2, new QTableWidgetItem(l.target));
        auto *app = new QComboBox;
        app->addItem(QIcon::fromTheme(QStringLiteral("internet-web-browser")), tr("Default"), QString());
        for (const KService::Ptr &b : browsers)
            app->addItem(QIcon::fromTheme(b->icon()), b->name(), b->storageId());
        app->setCurrentIndex(std::max(0, app->findData(l.app)));
        connect(app, &QComboBox::currentIndexChanged, this, [this] { saveQuickLinks(); });
        m_links->setCellWidget(row, 3, app);
    }
    m_loadingLinks = false;
}

void SettingsDialog::saveQuickLinks()
{
    QList<Library::QuickLink> links;
    for (int row = 0; row < m_links->rowCount(); ++row) {
        auto text = [&](int column) { return m_links->item(row, column) ? m_links->item(row, column)->text().trimmed() : QString(); };
        if (!text(0).isEmpty() && !text(2).isEmpty())
            links << Library::QuickLink{text(0), text(1), text(2),
                                        qobject_cast<QComboBox *>(m_links->cellWidget(row, 3)) ? qobject_cast<QComboBox *>(m_links->cellWidget(row, 3))->currentData().toString()
                                                                                                : QString()};
    }
    m_library->setQuickLinks(links);
}

// ------------------------------------------------------------- commands

QComboBox *SettingsDialog::outputBox(int output)
{
    auto *box = new QComboBox;
    box->addItem(tr("Ignore output"), int(Library::Command::Silent));
    box->addItem(tr("Show output"), int(Library::Command::Notify));
    box->addItem(tr("Copy output"), int(Library::Command::Copy));
    box->addItem(tr("Paste output"), int(Library::Command::Paste));
    box->addItem(tr("List lines as results"), int(Library::Command::List));
    box->setCurrentIndex(box->findData(output));
    connect(box, &QComboBox::currentIndexChanged, this, &SettingsDialog::saveCommands);
    return box;
}

QWidget *SettingsDialog::commandsPage()
{
    auto *page = new QWidget;
    m_commands = new QTableWidget(0, 4, page);
    m_commands->setHorizontalHeaderLabels({tr("Name"), tr("Keyword"), tr("Shell command"), tr("Output")});
    styleTable(m_commands, 2);
    m_commands->setColumnWidth(3, 150);
    m_loadingLinks = true;
    for (const Library::Command &c : m_library->commands()) {
        const int row = m_commands->rowCount();
        m_commands->insertRow(row);
        m_commands->setItem(row, 0, new QTableWidgetItem(c.name));
        m_commands->setItem(row, 1, new QTableWidgetItem(c.keyword));
        m_commands->setItem(row, 2, new QTableWidgetItem(c.command));
        m_commands->setCellWidget(row, 3, outputBox(c.output));
    }
    m_loadingLinks = false;
    connect(m_commands, &QTableWidget::itemChanged, this, [this] {
        if (!m_loadingLinks)
            saveCommands();
    });

    auto *add = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"), page);
    auto *remove = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), page);
    auto *extensions = new QPushButton(QIcon::fromTheme(QStringLiteral("folder-open")), tr("Open Extensions Folder"), page);
    connect(add, &QPushButton::clicked, this, [this] { showTab(Launcher::CommandsTab, true); });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (m_commands->currentRow() >= 0) {
            m_commands->removeRow(m_commands->currentRow());
            saveCommands();
        }
    });
    connect(extensions, &QPushButton::clicked, this, [] {
        (new KIO::OpenUrlJob(QUrl::fromLocalFile(ScriptSource::extensionsFolder())))->start();
    });
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(PageMargin, PageMargin - 4, PageMargin, PageMargin);
    layout->setSpacing(8);
    layout->addWidget(m_commands, 1);
    layout->addLayout(buttonRow(add, remove, extensions));
    layout->addWidget(note(tr("Commands run with sh. Put {query} in one to give it the text typed after its keyword "
                              "(quoted for you): keyword “k”, command “pkill -f {query}”, then type “k firefox”. "
                              "For several values use {argument name=\"to\"} (also default=\"…\", options=\"a,b\"): "
                              "Lumen asks for them in fields beside the search. "
                              "Extensions are programs in the extensions folder; see README.md for the format.")));

    // Raycast script commands: folders of scripts with @raycast metadata.
    layout->addSpacing(SectionGap);
    layout->addWidget(heading(tr("Raycast Script Commands"), true));
    auto *scriptFolders = new QListWidget(page);
    scriptFolders->setMaximumHeight(90);
    scriptFolders->addItems(ScriptCommandSource::folders());
    auto *addFolder = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add Folder…"), page);
    auto *removeFolder = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), page);
    auto saveFolders = [scriptFolders] {
        QStringList folders;
        for (int i = 0; i < scriptFolders->count(); ++i)
            folders << scriptFolders->item(i)->text();
        ScriptCommandSource::setFolders(folders);
    };
    connect(addFolder, &QPushButton::clicked, this, [this, scriptFolders, saveFolders] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder with script commands"), QDir::homePath());
        if (!folder.isEmpty() && scriptFolders->findItems(folder, Qt::MatchExactly).isEmpty()) {
            scriptFolders->addItem(folder);
            saveFolders();
        }
    });
    connect(removeFolder, &QPushButton::clicked, this, [scriptFolders, saveFolders] {
        delete scriptFolders->currentItem();
        saveFolders();
    });
    layout->addWidget(scriptFolders);
    layout->addLayout(buttonRow(addFolder, removeFolder));
    layout->addWidget(note(tr("Scripts written for Raycast run as they are: copy them from github.com/raycast/script-commands "
                              "into one of these folders (files named *.template.* are skipped).")));
    return page;
}

void SettingsDialog::saveCommands()
{
    QList<Library::Command> commands;
    for (int row = 0; row < m_commands->rowCount(); ++row) {
        auto text = [&](int column) { return m_commands->item(row, column) ? m_commands->item(row, column)->text().trimmed() : QString(); };
        const auto *box = qobject_cast<QComboBox *>(m_commands->cellWidget(row, 3));
        const auto output = Library::Command::Output(box ? box->currentData().toInt() : int(Library::Command::Notify));
        if (!text(0).isEmpty() && !text(2).isEmpty())
            commands << Library::Command{text(0), text(1), text(2), output};
    }
    m_library->setCommands(commands);
}

// ---------------------------------------------------------------- focus

QWidget *SettingsDialog::focusPage()
{
    auto *page = new QWidget;
    auto *form = ::form(page);
    auto focusConfig = [] { return KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Focus")); };

    form->addRow(heading(tr("Focus Sessions"), true));
    auto *minutes = new QSpinBox(page);
    minutes->setRange(5, 240);
    minutes->setSingleStep(5);
    minutes->setSuffix(tr(" minutes"));
    minutes->setValue(FocusMode::defaultMinutes());
    connect(minutes, &QSpinBox::valueChanged, this, [focusConfig](int value) {
        KConfigGroup g = focusConfig();
        g.writeEntry("Minutes", value);
        g.sync();
    });
    form->addRow(tr("Usual length:"), minutes);
    auto *dnd = new QCheckBox(tr("Hold notifications back (Do Not Disturb)"), page);
    dnd->setChecked(FocusMode::doNotDisturb());
    connect(dnd, &QCheckBox::toggled, this, [focusConfig](bool on) {
        KConfigGroup g = focusConfig();
        g.writeEntry("DoNotDisturb", on);
        g.sync();
    });
    form->addRow(QString(), dnd);

    form->addRow(heading(tr("Distractions")));
    auto *apps = new QListWidget(page);
    apps->setMaximumHeight(120);
    auto *appChoice = new QComboBox(page);
    const KService::List installed = KApplicationTrader::query([](const KService::Ptr &s) { return !s->noDisplay(); });
    QHash<QString, KService::Ptr> byId;
    for (const KService::Ptr &s : installed)
        byId.insert(s->desktopEntryName(), s);
    auto row = [byId](const QString &id) {
        const KService::Ptr s = byId.value(id.toLower());
        auto *item = new QListWidgetItem(s ? QIcon::fromTheme(s->icon()) : QIcon::fromTheme(QStringLiteral("application-x-executable")), s ? s->name() : id);
        item->setData(Qt::UserRole, id);
        return item;
    };
    for (const QString &id : FocusMode::blockedApps())
        if (byId.contains(id.toLower()))
            apps->addItem(row(id));
    KService::List sorted = installed;
    std::sort(sorted.begin(), sorted.end(), [](const KService::Ptr &a, const KService::Ptr &b) { return a->name().localeAwareCompare(b->name()) < 0; });
    for (const KService::Ptr &s : sorted)
        appChoice->addItem(QIcon::fromTheme(s->icon()), s->name(), s->desktopEntryName());
    // The list shows installed apps only; saving it drops defaults that are not installed.
    auto saveApps = [apps, focusConfig] {
        QStringList ids;
        for (int i = 0; i < apps->count(); ++i)
            ids << apps->item(i)->data(Qt::UserRole).toString();
        KConfigGroup g = focusConfig();
        g.writeEntry("Apps", ids);
        g.sync();
    };
    auto *addApp = new QPushButton(QIcon::fromTheme(QStringLiteral("list-add")), tr("Add"), page);
    auto *removeApp = new QPushButton(QIcon::fromTheme(QStringLiteral("list-remove")), tr("Remove"), page);
    connect(addApp, &QPushButton::clicked, this, [apps, appChoice, row, saveApps] {
        const QString id = appChoice->currentData().toString();
        for (int i = 0; i < apps->count(); ++i)
            if (apps->item(i)->data(Qt::UserRole).toString() == id)
                return;
        apps->addItem(row(id));
        saveApps();
    });
    connect(removeApp, &QPushButton::clicked, this, [apps, saveApps] {
        delete apps->currentItem();
        saveApps();
    });
    auto *appButtons = new QHBoxLayout;
    appButtons->setSpacing(8);
    appButtons->addWidget(appChoice, 1);
    appButtons->addWidget(addApp);
    appButtons->addWidget(removeApp);
    form->addRow(tr("Apps:"), apps);
    form->addRow(QString(), appButtons);

    auto *sites = new QPlainTextEdit(page);
    sites->setPlaceholderText(tr("One per line: youtube.com"));
    sites->setPlainText(FocusMode::blockedSites().join(u'\n'));
    sites->setFixedHeight(sites->fontMetrics().lineSpacing() * 5 + 16);
    connect(sites, &QPlainTextEdit::textChanged, this, [sites, focusConfig] {
        QStringList list;
        for (const QString &line : sites->toPlainText().split(u'\n'))
            if (!line.trimmed().isEmpty())
                list << line.trimmed();
        KConfigGroup g = focusConfig();
        g.writeEntry("Sites", list);
        g.sync();
    });
    form->addRow(tr("Websites:"), sites);
    form->addRow(QString(), note(tr("During a session these apps' windows are minimized whenever they come up, and so are "
                                    "browser windows showing these sites (recognized by name in the window title, e.g. "
                                    "“YouTube”). Type “focus” in Lumen to start one; changes apply to the next session.")));
    return page;
}

// --------------------------------------------------- calendar and weather

QWidget *SettingsDialog::calendarPage()
{
    auto *page = new QWidget;
    auto *form = ::form(page);

    form->addRow(heading(tr("Calendars"), true));
    auto *feeds = new QPlainTextEdit(page);
    feeds->setPlaceholderText(tr("One per line: https://…/basic.ics, webcal://…, or ~/calendar.ics"));
    feeds->setPlainText(CalendarSource::feeds().join(u'\n'));
    feeds->setFixedHeight(feeds->fontMetrics().lineSpacing() * 4 + 16);
    connect(feeds, &QPlainTextEdit::textChanged, this, [feeds] {
        QStringList list;
        for (const QString &line : feeds->toPlainText().split(u'\n')) {
            QString feed = line.trimmed();
            if (feed.startsWith(u'~'))
                feed = QDir::homePath() + feed.mid(1);
            if (!feed.isEmpty())
                list << feed;
        }
        CalendarSource::setFeeds(list);
    });
    form->addRow(tr("Feeds:"), feeds);
    form->addRow(QString(), note(tr("Google Calendar: Settings → your calendar → “Secret address in iCal format”. "
                                    "Outlook: Settings → Calendar → Shared calendars → Publish a calendar → ICS. "
                                    "Type “agenda” in Lumen to see what is coming up; the next meeting leads the start screen.")));

    form->addRow(heading(tr("Weather")));
    KConfigGroup weather = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Weather"));
    auto *place = new QLineEdit(weather.readEntry("Location"), page);
    place->setPlaceholderText(WeatherSource::defaultPlace());
    auto *units = new QComboBox(page);
    units->addItem(tr("As the system language suggests"), QStringLiteral("auto"));
    units->addItem(tr("Metric (°C, km/h)"), QStringLiteral("metric"));
    units->addItem(tr("Imperial (°F, mph)"), QStringLiteral("imperial"));
    units->setCurrentIndex(units->findData(weather.readEntry("Units", QStringLiteral("auto"))));
    auto save = [place, units] {
        KConfigGroup g = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Weather"));
        g.writeEntry("Location", place->text().trimmed());
        g.writeEntry("Units", units->currentData().toString());
        g.sync();
    };
    connect(place, &QLineEdit::editingFinished, this, save);
    connect(units, &QComboBox::currentIndexChanged, this, save);
    form->addRow(tr("Location:"), place);
    form->addRow(tr("Units:"), units);
    form->addRow(QString(), note(tr("Forecasts come from Open-Meteo (open-meteo.com), free and without an account. "
                                    "Type “weather” for the location above, or “weather tokyo” for anywhere else.")));
    return page;
}
