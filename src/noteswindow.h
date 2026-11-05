#pragma once

#include <QTimer>
#include <QWidget>

class QLabel;
class QPlainTextEdit;

// A small floating scratchpad, saved as you type to
// notes.md in the library folder (see Library). Esc hides it; the text stays.
//
// A normal window with the system's decorations, dressed like the
// launcher: the body is tinted glass with the accent wash and Lumen's beam
// along the top, the text sits on a paper well, and a footer keeps a word
// count beside the saving note.
class NotesWindow : public QWidget
{
    Q_OBJECT
public:
    explicit NotesWindow(QWidget *parent = nullptr);

    void showAndFocus();
    QString firstLine() const;
    static QString file();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void load();
    void save();
    void restyle(); // palette-dependent colours of the stock widgets
    void updateCount();

    QPlainTextEdit *m_text;
    QLabel *m_saveNote;
    QLabel *m_count;
    QTimer m_saveDelay;
};
