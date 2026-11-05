#pragma once

#include <QIcon>
#include <QObject>
#include <QSet>

class QNetworkAccessManager;

// Website icons for quick links: fetched once per site (Google's favicon
// service, 64 px) into ~/.cache/lumen/favicons, then read from there.
class Favicons : public QObject
{
    Q_OBJECT
public:
    static Favicons *instance();

    // The site's icon if it is known, else `fallback` (and the icon is
    // fetched; changed() says when it arrives).
    QIcon iconFor(const QString &url, const QIcon &fallback);

Q_SIGNALS:
    void changed();

private:
    Favicons();
    QString fileFor(const QString &host) const;

    QNetworkAccessManager *m_network = nullptr;
    QSet<QString> m_asked; // hosts fetched (or being fetched) this session
};
