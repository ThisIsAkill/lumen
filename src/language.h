#pragma once

#include "source.h"

#include <QPointer>
#include <QTimer>

#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

// Words, in the main search:
//  - Translate: "translate bonjour", "tr guten morgen", "hello in spanish",
//    "translate good night to japanese". The language of the text is found
//    by itself; the target is lumenrc [Translate] Target (the system
//    language by default; text already in it goes to English).
//  - Dictionary (English): "define serendipity"; Enter shows the whole entry.
//  - "synonyms for happy", "rhymes with orange".
// Translations come from Google Translate's free web endpoint, definitions
// from dictionaryapi.dev and word lists from Datamuse: no accounts or keys.
class LanguageSource : public Source
{
    Q_OBJECT
public:
    explicit LanguageSource(Launcher *launcher);

    void query(const QString &text) override;

    struct Request {
        enum Kind { Translate, Define, Synonyms, Rhymes } kind = Translate;
        QString text;
        QString target; // a language code, for Translate; empty for the default
        bool explicitly = false; // said "translate"/"define": the main answer
    };
    // What a query asks for, if anything (tested without the network).
    static std::optional<Request> parse(const QString &query);
    // "german", "Deutsch", "de" → "de"; "" when not a language.
    static QString languageCode(const QString &name);
    static QString languageName(const QString &code);
    // The pieces of the translate endpoint's answer: text, source language.
    static std::pair<QString, QString> parseTranslation(const QByteArray &json);

private:
    void fetch();
    void translate(const Request &request, const QString &target, bool retried);
    void publish();

    QNetworkAccessManager *m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_delay;
    QString m_query;
    std::optional<Request> m_request;
    QList<Item> m_results; // for m_resultsFor
    QString m_resultsFor;
};
