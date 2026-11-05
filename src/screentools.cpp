#include "screentools.h"
#include "clipboardhistory.h"
#include "fuzzy.h"
#include "launcher.h"

#include <KConfigGroup>
#include <KIO/OpenUrlJob>
#include <KSharedConfig>

#include <ZXing/CreateBarcode.h>
#include <ZXing/ReadBarcode.h>
#include <ZXing/WriteBarcode.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace
{

QString runtimeFile(const QString &name)
{
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/lumen-") + name;
}

QString clipboardText(Launcher *launcher)
{
    for (const ClipboardHistory::Entry &e : launcher->clipboardHistory()->entries())
        if (!e.text.isEmpty())
            return e.text;
    return {};
}

Item command(const QString &id, const QString &title, const QString &subtitle, const QString &icon, qreal score, const Action &action)
{
    Item item;
    item.id = QStringLiteral("lumen:") + id;
    item.title = title;
    item.subtitle = subtitle;
    item.kind = QObject::tr("Command");
    item.icon = QIcon::fromTheme(icon);
    item.score = score;
    item.actions << action;
    return item;
}

} // namespace

QImage ScreenTools::makeQr(const QString &text, int scale)
{
    try {
        const ZXing::Barcode code = ZXing::CreateBarcodeFromText(text.toStdString(), ZXing::CreatorOptions(ZXing::BarcodeFormat::QRCode, "ecLevel=M"));
        const ZXing::Image image = ZXing::WriteBarcodeToImage(code, ZXing::WriterOptions().scale(scale).addQuietZones(true));
        return QImage(image.data(), image.width(), image.height(), image.rowStride(), QImage::Format_Grayscale8).copy();
    } catch (const std::exception &) {
        return {}; // too long for a QR code
    }
}

QStringList ScreenTools::readCodes(const QImage &source)
{
    const QImage image = source.convertToFormat(QImage::Format_Grayscale8);
    const ZXing::ImageView view(image.constBits(), image.width(), image.height(), ZXing::ImageFormat::Lum, int(image.bytesPerLine()));
    QStringList texts;
    for (const ZXing::Barcode &code : ZXing::ReadBarcodes(view))
        texts << QString::fromStdString(code.text());
    return texts;
}

QStringList ScreenTools::ocrLanguages()
{
    QProcess proc;
    proc.start(QStringLiteral("tesseract"), {QStringLiteral("--list-langs")});
    if (!proc.waitForFinished(3000))
        return {};
    QStringList langs = QString::fromUtf8(proc.readAllStandardOutput()).split(u'\n', Qt::SkipEmptyParts).mid(1); // after the heading
    langs.removeAll(QStringLiteral("osd"));
    return langs;
}

void ScreenTools::captureArea(std::function<void(const QImage &)> done)
{
    const QString file = runtimeFile(QStringLiteral("capture.png"));
    QFile::remove(file);
    auto *proc = new QProcess(this);
    connect(proc, &QProcess::finished, this, [proc, file, done] {
        proc->deleteLater();
        const QImage image(file); // missing when the selection was cancelled
        QFile::remove(file);
        done(image);
    });
    // Lumen closes first so it is not in the picture.
    launcher()->runAfterHide([proc, file] {
        proc->start(QStringLiteral("spectacle"), {QStringLiteral("-b"), QStringLiteral("-n"), QStringLiteral("-r"), QStringLiteral("-o"), file});
    });
}

void ScreenTools::copyTextFromScreen()
{
    // The languages to read: lumenrc [OCR] Languages ("eng+deu"), or English.
    const QString wanted = KSharedConfig::openConfig(QStringLiteral("lumenrc"))->group(QStringLiteral("OCR")).readEntry("Languages", QStringLiteral("eng"));
    const QStringList installed = ocrLanguages();
    for (const QString &lang : wanted.split(u'+')) {
        if (!installed.contains(lang)) {
            launcher()->notify(installed.isEmpty() ? tr("Needs tesseract: sudo pacman -S tesseract tesseract-data-eng")
                                                   : tr("Needs the %1 language data: sudo pacman -S tesseract-data-%1").arg(lang));
            return;
        }
    }
    Launcher *l = launcher();
    captureArea([this, l, wanted](const QImage &image) {
        if (image.isNull())
            return;
        const QString file = runtimeFile(QStringLiteral("ocr.png"));
        image.save(file);
        auto *proc = new QProcess(this);
        connect(proc, &QProcess::finished, this, [proc, l, file] {
            proc->deleteLater();
            QFile::remove(file);
            const QString text = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
            if (text.isEmpty())
                l->notify(tr("No text found"));
            else
                l->copyText(text);
        });
        proc->start(QStringLiteral("tesseract"), {file, QStringLiteral("-"), QStringLiteral("-l"), wanted});
    });
}

void ScreenTools::scanQr()
{
    Launcher *l = launcher();
    captureArea([l](const QImage &image) {
        if (image.isNull())
            return;
        const QStringList codes = readCodes(image);
        if (codes.isEmpty()) {
            l->notify(tr("No QR code found there"));
            return;
        }
        const QString text = codes.first();
        const QUrl url(text);
        if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
            (new KIO::OpenUrlJob(url))->start();
        else
            l->copyText(text);
    });
}

void ScreenTools::query(const QString &text)
{
    const QString q = text.trimmed();
    QList<Item> items;
    if (q.isEmpty()) {
        Q_EMIT resultsReady(items);
        return;
    }
    Launcher *l = launcher();
    auto self = this;

    const qreal ocr = std::max({fuzzyScore(q, QStringLiteral("Copy Text from Screen")), fuzzyScore(q, QStringLiteral("ocr")),
                                fuzzyScore(q, QStringLiteral("text from screen"))});
    if (ocr >= 40)
        items << command(QStringLiteral("ocr"), tr("Copy Text from Screen"), tr("Select an area; its text goes to the clipboard"),
                         QStringLiteral("edit-select-text"), ocr, {tr("Select Area"), {}, {}, [self] { self->copyTextFromScreen(); }, true});
    const qreal scan = std::max(fuzzyScore(q, QStringLiteral("Scan QR Code")), fuzzyScore(q, QStringLiteral("read qr")));
    if (scan >= 40)
        items << command(QStringLiteral("scan-qr"), tr("Scan QR Code"), tr("Select an area with a QR code"), QStringLiteral("view-barcode-qr"),
                         scan, {tr("Select Area"), {}, {}, [self] { self->scanQr(); }, true});

    // "qr TEXT" or "qr" (the clipboard)
    if (q.compare(QLatin1String("qr"), Qt::CaseInsensitive) == 0 || q.startsWith(QLatin1String("qr "), Qt::CaseInsensitive)) {
        const QString content = q.size() > 3 ? q.mid(3) : clipboardText(l);
        const QImage code = content.isEmpty() ? QImage() : makeQr(content);
        if (!code.isNull()) {
            const QString file = runtimeFile(QStringLiteral("qr-") + QString::fromLatin1(QCryptographicHash::hash(content.toUtf8(), QCryptographicHash::Sha1).toHex().left(12))
                                             + QStringLiteral(".png"));
            code.save(file);
            Item item;
            item.id = QStringLiteral("answer:qr");
            item.title = tr("QR Code");
            item.subtitle = content.simplified().left(80) + (q.size() > 3 ? QString() : tr(" (clipboard)"));
            item.kind = tr("Space to view");
            item.icon = QIcon(file);
            item.filePath = file; // Space / Ctrl+Y previews it
            item.score = 115;
            item.actions << Action{tr("Copy Image"), {}, {}, [l, file] { l->copyImage(file); }};
            item.actions << Action{tr("Save to Pictures"), QStringLiteral("document-save"), QKeySequence(Qt::CTRL | Qt::Key_S), [l, file] {
                const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
                const QString target = QDir(dir).filePath(QStringLiteral("QR code %1.png").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh.mm.ss"))));
                l->notify(QFile::copy(file, target) ? tr("Saved to Pictures") : tr("Could not save"));
            }};
            items << item;
        }
    }
    Q_EMIT resultsReady(items);
}
