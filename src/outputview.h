#pragma once

#include "source.h"

#include <QPointer>
#include <QProcess>

// A view showing what a program printed (a Raycast script command in
// fullOutput mode, an extension's detail): one row per line, the whole
// output beside them, Enter copies it. Markdown is shown rendered, as one
// row. Output can arrive while the view is open.
class OutputView : public Source
{
    Q_OBJECT
public:
    OutputView(Launcher *launcher, const QString &title, const QString &icon);
    ~OutputView() override;

    void setText(const QString &text, bool markdown = false);
    // Shows the program's output (both channels) as it arrives; the
    // program is stopped if the view is left first.
    void follow(QProcess *process);

    void query(const QString &text) override;
    QString viewTitle() const override { return m_title; }
    QString viewPlaceholder() const override { return tr("Filter output…"); }
    bool hasPreviewPane() const override { return true; }

    // Without terminal colour codes.
    static QString plain(const QString &output);

private:
    QString m_title;
    QString m_icon;
    QString m_text;
    bool m_markdown = false;
    bool m_running = false;
    QString m_query;
    QPointer<QProcess> m_process;
};
