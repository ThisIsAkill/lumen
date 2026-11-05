#pragma once

#include "source.h"

#include <KService>

// Applications found despite a typo ("firfox" → Firefox). KRunner finds
// correctly spelt names itself; this only adds what it misses.
class AppSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;

    void query(const QString &text) override;
    void sessionStarted() override;

private:
    KService::List m_apps;
};
