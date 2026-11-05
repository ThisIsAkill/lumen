#include "favicons.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>

Favicons *Favicons::instance()
{
    static Favicons *favicons = new Favicons;
    return favicons;
}

Favicons::Favicons()
    : m_network(new QNetworkAccessManager(this))
{
    QDir().mkpath(fileFor({}).section(u'/', 0, -2));
}

QString Favicons::fileFor(const QString &host) const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/lumen/favicons/") + host + QStringLiteral(".png");
}

QIcon Favicons::iconFor(const QString &url, const QIcon &fallback)
{
    // "{query}" in a template is not part of the address.
    const QString host = QUrl::fromUserInput(QString(url).replace(QLatin1String("{query}"), QLatin1String("x"))).host().toLower();
    if (host.isEmpty())
        return fallback;
    const QString file = fileFor(host);
    if (QFile::exists(file))
        return QIcon(file);
    if (!m_asked.contains(host)) {
        m_asked.insert(host);
        QUrl source(QStringLiteral("https://www.google.com/s2/favicons"));
        source.setQuery({{QStringLiteral("domain"), host}, {QStringLiteral("sz"), QStringLiteral("64")}});
        QNetworkRequest request(source);
        request.setTransferTimeout(6000);
        QNetworkReply *reply = m_network->get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, file] {
            reply->deleteLater();
            const QImage image = QImage::fromData(reply->readAll());
            // A tiny or missing image means the site has none: keep the globe.
            if (reply->error() == QNetworkReply::NoError && image.width() >= 16 && image.save(file))
                Q_EMIT changed();
        });
    }
    return fallback;
}
