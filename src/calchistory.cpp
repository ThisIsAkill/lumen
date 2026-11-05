#include "calchistory.h"
#include "launcher.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSaveFile>
#include <QStandardPaths>

namespace
{

constexpr int MaxKept = 200;

QString file()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/lumen/calculations.json");
}

} // namespace

bool CalcHistory::isCalculation(const QString &id, const QString &kind)
{
    // KRunner's calculator and unit converter, and Lumen's percentages.
    static const QStringList kinds{QStringLiteral("Calculator"), QStringLiteral("Unit Conversion"), QStringLiteral("Unit Converter")};
    return id == QLatin1String("answer:percent") || kinds.contains(kind.section(QStringLiteral(" · "), 0, 0));
}

QList<CalcHistory::Calculation> CalcHistory::entries()
{
    QFile f(file());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QList<Calculation> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(f.readAll()).array()) {
        const QJsonObject o = v.toObject();
        out << Calculation{o.value(QStringLiteral("expression")).toString(), o.value(QStringLiteral("result")).toString(),
                           QDateTime::fromSecsSinceEpoch(o.value(QStringLiteral("time")).toInteger())};
    }
    return out;
}

void CalcHistory::save(const QList<Calculation> &entries)
{
    QJsonArray list;
    for (const Calculation &c : entries)
        list << QJsonObject{{QStringLiteral("expression"), c.expression}, {QStringLiteral("result"), c.result},
                            {QStringLiteral("time"), c.time.toSecsSinceEpoch()}};
    QDir().mkpath(QFileInfo(file()).path());
    QSaveFile f(file());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(list).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void CalcHistory::record(const QString &expression, const QString &result)
{
    if (expression.trimmed().isEmpty() || result.trimmed().isEmpty())
        return;
    QList<Calculation> all = entries();
    all.removeIf([&](const Calculation &c) { return c.expression == expression.trimmed(); });
    all.prepend({expression.trimmed(), result.trimmed(), QDateTime::currentDateTime()});
    if (all.size() > MaxKept)
        all.resize(MaxKept);
    save(all);
}

void CalcHistory::remove(const QString &expression)
{
    QList<Calculation> all = entries();
    all.removeIf([&](const Calculation &c) { return c.expression == expression; });
    save(all);
}

void CalcHistory::clear()
{
    save({});
}

void CalcHistory::query(const QString &text)
{
    const QString q = text.trimmed();
    Launcher *l = launcher();
    auto self = this;
    QList<Item> items;
    const QList<Calculation> all = entries();
    for (int i = 0; i < all.size(); ++i) {
        const Calculation &c = all.at(i);
        if (!q.isEmpty() && !c.expression.contains(q, Qt::CaseInsensitive) && !c.result.contains(q, Qt::CaseInsensitive))
            continue;
        Item item;
        item.id = QStringLiteral("calculation:") + c.expression;
        item.title = c.result;
        item.subtitle = c.expression;
        item.kind = QLocale().toString(c.time, QLocale::ShortFormat);
        item.icon = QIcon::fromTheme(QStringLiteral("accessories-calculator"));
        item.score = 1000 - i;
        const QString result = c.result, expression = c.expression;
        if (l->canPaste())
            item.actions << Action{tr("Paste Result"), {}, {}, [l, result] { l->pasteText(result); }};
        item.actions << Action{tr("Copy Result"), QStringLiteral("edit-copy"), l->canPaste() ? QKeySequence(Qt::CTRL | Qt::Key_Return) : QKeySequence(),
                               [l, result] { l->copyText(result); }};
        item.actions << Action{tr("Calculate Again"), QStringLiteral("edit-redo"), QKeySequence(Qt::CTRL | Qt::Key_E),
                               [l, expression] { l->openView(Launcher::RootView, expression); }, true};
        item.actions << Action{tr("Delete"), QStringLiteral("edit-delete"), QKeySequence(Qt::CTRL | Qt::Key_Backspace), [self, expression, text] {
            remove(expression);
            self->query(text);
        }, true};
        item.actions << Action{tr("Delete All"), QStringLiteral("edit-clear-history"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Backspace),
                               [self, text] {
                                   clear();
                                   self->query(text);
                               },
                               true};
        items << item;
    }
    if (items.isEmpty() && all.isEmpty()) {
        Item none;
        none.title = tr("No calculations yet");
        none.subtitle = tr("Type one in the search (“15% of 80”, “3 ft in cm”) and copy the answer");
        none.icon = QIcon::fromTheme(QStringLiteral("accessories-calculator"));
        items << none;
    }
    Q_EMIT resultsReady(items);
}
