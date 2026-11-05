#pragma once

#include "source.h"

#include <QDateTime>

// Calculations and conversions that were used (copied, pasted) from the
// main search, kept in ~/.local/share/lumen/calculations.json, newest first.
// The "Calculator History" view lists them to copy a result again or reuse
// the expression.
class CalcHistory : public Source
{
    Q_OBJECT
public:
    struct Calculation {
        QString expression;
        QString result;
        QDateTime time;
    };

    using Source::Source;
    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Calculations"); }
    QString viewPlaceholder() const override { return tr("Search past calculations…"); }

    static QList<Calculation> entries();
    static void record(const QString &expression, const QString &result);
    static void remove(const QString &expression);
    static void clear();
    // Whether a result is a calculation worth keeping (by its kind).
    static bool isCalculation(const QString &id, const QString &kind);

private:
    static void save(const QList<Calculation> &entries);
};
