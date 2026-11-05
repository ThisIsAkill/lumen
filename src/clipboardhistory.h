#pragma once

#include "source.h"

#include <QDateTime>
#include <QImage>
#include <QProcess>
#include <QTimer>

// Keeps a history of copied text and images in
// ~/.local/share/lumen/clipboard/. On Wayland it listens through wl-paste
// (wl-clipboard), which can read the clipboard without having focus; on X11
// it uses Qt's clipboard. Content marked secret by password managers
// (x-kde-passwordManagerHint) is never stored.
//
// Text in copied images is recognized in the background (Tesseract, at the
// lowest priority, one image at a time) so the history finds them by it.
class ClipboardHistory : public QObject
{
    Q_OBJECT
public:
    struct Entry {
        QString text;      // empty for images
        QString imageFile; // file name inside the history folder
        QSize imageSize;
        QDateTime time;
        bool pinned = false;
        QString imageText;      // text recognized in an image
        bool recognized = false; // recognition has run (it may find nothing)
    };

    explicit ClipboardHistory(QObject *parent = nullptr);
    ~ClipboardHistory() override;

    void setEnabled(bool enabled);
    void setLimit(int limit);

    QList<Entry> entries() const { return m_entries; }
    QString imagePath(const Entry &entry) const;
    void remove(int index);
    void setPinned(int index, bool pinned);
    void clearUnpinned();

    // The program that reads text in an image, "{image}" standing for its
    // path, printing the text; empty turns recognition off. By default
    // Tesseract with lumenrc [OCR] Languages, when installed.
    void setRecognizer(const QStringList &command);
    static QStringList defaultRecognizer();

Q_SIGNALS:
    void changed();
    void added(const ClipboardHistory::Entry &entry); // something new was copied

private:
    void startWatching();
    void stopWatching();
    void fetchWayland();
    void readQtClipboard();
    void addText(const QString &text);
    void addImage(const QImage &image);
    void addEntry(const Entry &entry);
    void trim();
    void load();
    void save() const;
    void recognizeNext();

    QString m_dir;
    QList<Entry> m_entries;
    int m_limit = 200;
    bool m_enabled = true;
    QProcess *m_watcher = nullptr;
    QTimer m_fetchDelay;
    QStringList m_recognizer;
    QStringList m_toRecognize; // image files, next first
    QProcess *m_recognizing = nullptr;
    bool m_recognizerSet = false; // chosen (by the default, a test or the settings)
};

// The "Clipboard History" view.
class ClipboardSource : public Source
{
    Q_OBJECT
public:
    ClipboardSource(Launcher *launcher, ClipboardHistory *history);

    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Clipboard"); }
    QString viewPlaceholder() const override { return tr("Search clipboard history…"); }
    bool hasPreviewPane() const override { return true; }

private:
    ClipboardHistory *m_history;
    QString m_lastQuery;
};
