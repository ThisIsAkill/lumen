#pragma once

#include "source.h"

// Instant answers at the top of the main search: time zones, date maths and
// colours (see answers.h). Unit and currency conversion come from KRunner.
class AnswerSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;
};
