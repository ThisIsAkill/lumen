#include "language.h"
#include "launcher.h"
#include "outputview.h"

#include <KConfigGroup>
#include <KSharedConfig>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrlQuery>

namespace
{

constexpr int Timeout = 6000; // ms for a web answer
constexpr int MaxWords = 24;

// Language names (English and native) and codes → the code Google uses.
const QHash<QString, QString> &languages()
{
    static const QHash<QString, QString> table = [] {
        QHash<QString, QString> t;
        for (int l = QLocale::Abkhazian; l <= QLocale::LastLanguage; ++l) {
            const QLocale locale(QLocale::Language(l), QLocale::AnyTerritory);
            if (locale.language() != QLocale::Language(l))
                continue; // no data for it
            QString code = locale.name().section(u'_', 0, 0);
            if (code.size() > 3 || code == QLatin1String("C"))
                continue;
            if (code == QLatin1String("zh"))
                code = QStringLiteral("zh-CN");
            t.insert(QLocale::languageToString(QLocale::Language(l)).toLower(), code);
            t.insert(locale.nativeLanguageName().toLower(), code);
            t.insert(code.toLower(), code);
        }
        t.insert(QStringLiteral("chinese"), QStringLiteral("zh-CN"));
        t.insert(QStringLiteral("mandarin"), QStringLiteral("zh-CN"));
        t.insert(QStringLiteral("traditional chinese"), QStringLiteral("zh-TW"));
        t.insert(QStringLiteral("farsi"), QStringLiteral("fa"));
        t.remove(QStringLiteral("in")); // a word, not Indonesian's old code
        t.remove(QStringLiteral("to"));
        t.remove(QStringLiteral("it")); // too common a word to mean Italian
        return t;
    }();
    return table;
}

QString defaultTarget()
{
    const QString configured = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("Translate")).readEntry("Target");
    return configured.isEmpty() ? QLocale().name().section(u'_', 0, 0) : configured;
}

Item answer(const QString &id, const QString &title, const QString &subtitle, const QString &icon)
{
    Item item;
    item.id = QStringLiteral("answer:") + id;
    item.title = title;
    item.subtitle = subtitle;
    item.kind = QObject::tr("Answer");
    // Not every icon theme has a dictionary: a book or help icon instead.
    item.icon = QIcon::fromTheme(icon, QIcon::fromTheme(QStringLiteral("dictionary"), QIcon::fromTheme(QStringLiteral("help-contents"))));
    item.score = 114;
    return item;
}

} // namespace

LanguageSource::LanguageSource(Launcher *launcher)
    : Source(launcher)
    , m_network(new QNetworkAccessManager(this))
{
    m_delay.setSingleShot(true);
    m_delay.setInterval(350); // after a typing pause: one request, not one per key
    connect(&m_delay, &QTimer::timeout, this, &LanguageSource::fetch);
}

QString LanguageSource::languageCode(const QString &name)
{
    return languages().value(name.trimmed().toLower());
}

QString LanguageSource::languageName(const QString &code)
{
    if (code.startsWith(QLatin1String("zh")))
        return QObject::tr("Chinese");
    return QLocale::languageToString(QLocale(code).language());
}

std::optional<LanguageSource::Request> LanguageSource::parse(const QString &query)
{
    const QString q = query.trimmed();
    const auto flags = QRegularExpression::CaseInsensitiveOption;
    QRegularExpressionMatch m;
    // "translate X", "tr X", with "to/in/into LANGUAGE" at the end.
    static const QRegularExpression translate(QStringLiteral(R"(^(?:translate|tr)\s+(.+?)(?:\s+(?:to|in|into)\s+([\p{L} ]+))?$)"), flags);
    if ((m = translate.match(q)).hasMatch()) {
        Request r;
        r.explicitly = true;
        const QString code = m.captured(2).isEmpty() ? QString() : languageCode(m.captured(2));
        r.text = code.isEmpty() && !m.captured(2).isEmpty() ? m.captured(0).section(u' ', 1) : m.captured(1);
        r.target = code;
        return r;
    }
    static const QRegularExpression define(QStringLiteral(R"(^(?:define|definition of|meaning of|what does)\s+([\p{L}'-]+)(?:\s+mean)?\??$)"), flags);
    static const QRegularExpression defineAfter(QStringLiteral(R"(^([\p{L}'-]+)\s+(?:definition|meaning)$)"), flags);
    if ((m = define.match(q)).hasMatch() || (m = defineAfter.match(q)).hasMatch())
        return Request{Request::Define, m.captured(1), {}, true};
    static const QRegularExpression synonyms(QStringLiteral(R"(^(?:synonyms?(?:\s+(?:for|of))?|another word for)\s+([\p{L} '-]+)$)"), flags);
    if ((m = synonyms.match(q)).hasMatch())
        return Request{Request::Synonyms, m.captured(1).trimmed(), {}, true};
    static const QRegularExpression rhymes(QStringLiteral(R"(^(?:rhymes?|rhyming)(?:\s+(?:with|for))?\s+([\p{L}'-]+)$)"), flags);
    if ((m = rhymes.match(q)).hasMatch())
        return Request{Request::Rhymes, m.captured(1), {}, true};
    // "good morning in japanese": words (not numbers or units) and a language.
    static const QRegularExpression inLanguage(QStringLiteral(R"(^(.*\p{L}.*?)\s+(?:in|to)\s+([\p{L}]+)$)"), flags);
    if ((m = inLanguage.match(q)).hasMatch() && !m.captured(1).contains(QRegularExpression(QStringLiteral("\\d")))) {
        if (const QString code = languageCode(m.captured(2)); !code.isEmpty())
            return Request{Request::Translate, m.captured(1), code, false};
    }
    return std::nullopt;
}

std::pair<QString, QString> LanguageSource::parseTranslation(const QByteArray &json)
{
    // [[["Hallo Welt","Hello world",…],…], null, "en", …]
    const QJsonArray root = QJsonDocument::fromJson(json).array();
    QString text;
    for (const QJsonValue &segment : root.at(0).toArray())
        text += segment.toArray().at(0).toString();
    return {text, root.at(2).toString()};
}

void LanguageSource::query(const QString &text)
{
    m_query = text.trimmed();
    m_request = parse(m_query);
    if (!m_request || m_request->text.trimmed().isEmpty()) {
        Q_EMIT resultsReady({});
        return;
    }
    m_delay.start();
    publish(); // earlier results for the same text, or nothing yet
}

void LanguageSource::fetch()
{
    if (!m_request)
        return;
    if (m_reply)
        m_reply->abort();
    const Request r = *m_request;
    if (r.kind == Request::Translate) {
        translate(r, r.target.isEmpty() ? defaultTarget() : r.target, false);
        return;
    }
    QUrl url;
    if (r.kind == Request::Define) {
        url = QUrl(QStringLiteral("https://api.dictionaryapi.dev/api/v2/entries/en/") + QString::fromUtf8(QUrl::toPercentEncoding(r.text.toLower())));
    } else {
        url = QUrl(QStringLiteral("https://api.datamuse.com/words"));
        url.setQuery({{r.kind == Request::Synonyms ? QStringLiteral("rel_syn") : QStringLiteral("rel_rhy"), r.text.toLower()},
                      {QStringLiteral("max"), QString::number(MaxWords)}});
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(Timeout);
    m_reply = m_network->get(request);
    const QString forQuery = m_query;
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply.data(), r, forQuery] {
        reply->deleteLater();
        if (forQuery != m_query || reply->error() == QNetworkReply::OperationCanceledError)
            return;
        Launcher *l = launcher();
        QList<Item> items;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (r.kind == Request::Define) {
            const QJsonObject entry = doc.array().at(0).toObject();
            QString markdown = QStringLiteral("# %1\n").arg(entry.value(QStringLiteral("word")).toString(r.text));
            const QString phonetic = entry.value(QStringLiteral("phonetic")).toString();
            if (!phonetic.isEmpty())
                markdown += QStringLiteral("*%1*\n").arg(phonetic);
            QStringList synonyms;
            int n = 0;
            for (const QJsonValue &mv : entry.value(QStringLiteral("meanings")).toArray()) {
                const QJsonObject meaning = mv.toObject();
                const QString part = meaning.value(QStringLiteral("partOfSpeech")).toString();
                markdown += QStringLiteral("\n## %1\n").arg(part);
                int i = 1;
                for (const QJsonValue &dv : meaning.value(QStringLiteral("definitions")).toArray()) {
                    const QJsonObject d = dv.toObject();
                    const QString definition = d.value(QStringLiteral("definition")).toString();
                    markdown += QStringLiteral("%1. %2\n").arg(i++).arg(definition);
                    if (const QString example = d.value(QStringLiteral("example")).toString(); !example.isEmpty())
                        markdown += QStringLiteral("   > %1\n").arg(example);
                    if (n < 4) {
                        Item item = n == 0 ? answer(QStringLiteral("define"), definition, {}, QStringLiteral("accessories-dictionary"))
                                           : answer(QStringLiteral("define-%1").arg(n), definition, {}, QStringLiteral("accessories-dictionary"));
                        item.subtitle = n == 0 ? QStringList{entry.value(QStringLiteral("word")).toString(), phonetic, part}.join(QStringLiteral(" · "))
                                               : part;
                        item.score -= n * 0.5;
                        items << item;
                    }
                    ++n;
                }
                for (const QJsonValue &s : meaning.value(QStringLiteral("synonyms")).toArray())
                    if (!synonyms.contains(s.toString()))
                        synonyms << s.toString();
            }
            if (!synonyms.isEmpty())
                markdown += QStringLiteral("\n**Synonyms:** %1\n").arg(synonyms.mid(0, 12).join(QStringLiteral(", ")));
            if (items.isEmpty()) {
                items << answer(QStringLiteral("define"), tr("No definition for “%1”").arg(r.text), tr("English dictionary"),
                                QStringLiteral("accessories-dictionary"));
            } else {
                const QString word = entry.value(QStringLiteral("word")).toString(r.text);
                for (Item &item : items) {
                    const QString definition = item.title;
                    item.actions << Action{tr("Show Entry"), {}, {}, [l, word, markdown] {
                        auto *view = new OutputView(l, word, QStringLiteral("accessories-dictionary"));
                        view->setText(markdown, true);
                        l->pushView(view);
                    }, true};
                    item.actions << Action{tr("Copy Definition"), QStringLiteral("edit-copy"), QKeySequence(Qt::CTRL | Qt::Key_Return),
                                           [l, definition] { l->copyText(definition); }};
                }
            }
        } else {
            QStringList words;
            for (const QJsonValue &v : doc.array())
                words << v.toObject().value(QStringLiteral("word")).toString();
            const QString what = r.kind == Request::Synonyms ? tr("Synonyms for “%1”").arg(r.text) : tr("Rhymes with “%1”").arg(r.text);
            if (words.isEmpty()) {
                items << answer(QStringLiteral("words"), tr("Nothing found"), what, QStringLiteral("accessories-dictionary"));
            } else {
                Item card = answer(QStringLiteral("words"), words.mid(0, 8).join(QStringLiteral(", ")), what, QStringLiteral("accessories-dictionary"));
                const QString all = words.join(QStringLiteral(", "));
                card.actions << Action{tr("Copy All"), {}, {}, [l, all] { l->copyText(all); }};
                items << card;
                for (int i = 0; i < words.size(); ++i) {
                    Item item;
                    item.id = QStringLiteral("word:") + words.at(i);
                    item.title = words.at(i);
                    item.kind = r.kind == Request::Synonyms ? tr("Synonym") : tr("Rhyme");
                    item.icon = QIcon::fromTheme(QStringLiteral("insert-text"));
                    item.score = 105 - i * 0.01;
                    const QString word = words.at(i);
                    item.actions << Action{tr("Copy"), {}, {}, [l, word] { l->copyText(word); }};
                    items << item;
                }
            }
        }
        m_results = items;
        m_resultsFor = forQuery;
        publish();
    });
}

void LanguageSource::translate(const Request &r, const QString &target, bool retried)
{
    QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"));
    url.setQuery({{QStringLiteral("client"), QStringLiteral("gtx")},
                  {QStringLiteral("sl"), QStringLiteral("auto")},
                  {QStringLiteral("tl"), target},
                  {QStringLiteral("dt"), QStringLiteral("t")},
                  {QStringLiteral("q"), r.text}});
    QNetworkRequest request(url);
    request.setTransferTimeout(Timeout);
    m_reply = m_network->get(request);
    const QString forQuery = m_query;
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply.data(), r, target, retried, forQuery] {
        reply->deleteLater();
        if (forQuery != m_query || reply->error() == QNetworkReply::OperationCanceledError)
            return;
        const auto [text, source] = parseTranslation(reply->readAll());
        // Already in the default language: English is the more useful answer.
        if (!retried && r.target.isEmpty() && source == target && target != QLatin1String("en")) {
            translate(r, QStringLiteral("en"), true);
            return;
        }
        Launcher *l = launcher();
        QList<Item> items;
        if (text.isEmpty()) {
            items << answer(QStringLiteral("translate"), tr("Could not translate"), tr("Google Translate did not answer"),
                            QStringLiteral("preferences-desktop-locale"));
        } else {
            Item item = answer(QStringLiteral("translate"), text, tr("%1 → %2").arg(languageName(source), languageName(target)),
                               QStringLiteral("preferences-desktop-locale"));
            if (!r.explicitly)
                item.score = 100; // "… in french" could also mean something else
            item.previewText = text;
            const QString original = r.text;
            if (l->canPaste())
                item.actions << Action{tr("Paste Translation"), {}, {}, [l, text] { l->pasteText(text); }};
            item.actions << Action{tr("Copy Translation"), QStringLiteral("edit-copy"), l->canPaste() ? QKeySequence(Qt::CTRL | Qt::Key_Return) : QKeySequence(),
                                   [l, text] { l->copyText(text); }};
            const QUrl page(QStringLiteral("https://translate.google.com/?sl=auto&tl=%1&text=%2")
                                .arg(target, QString::fromUtf8(QUrl::toPercentEncoding(original))));
            item.actions << Action{tr("Open in Google Translate"), QStringLiteral("internet-web-browser"), QKeySequence(Qt::CTRL | Qt::Key_O),
                                   [l, page] { l->openUrl(page); }};
            items << item;
        }
        m_results = items;
        m_resultsFor = forQuery;
        publish();
    });
}

void LanguageSource::publish()
{
    Q_EMIT resultsReady(m_resultsFor == m_query ? m_results : QList<Item>());
}
