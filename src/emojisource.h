#pragma once

#include "source.h"

// The "Emoji" view: search emoji and symbols by name and keyword ("shrug",
// "arrow", "heart"), using Plasma's emoji data (the same as its Emoji
// Selector), in the user's language when available.
class EmojiSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;

    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Emoji"); }
    QString viewPlaceholder() const override { return tr("Search emoji and symbols…"); }

    struct Emoji {
        QString character;
        QString name;
        QStringList keywords;
    };

    // Reads Plasma's emoji dictionary (a qCompress'ed list of entries).
    static QList<Emoji> load(const QString &file);

private:
    QList<Emoji> m_emoji; // loaded on first use
};
