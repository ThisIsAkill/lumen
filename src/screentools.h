#pragma once

#include "source.h"

#include <QImage>

// Screen and QR tools in the main search:
//  - "Copy Text from Screen" (OCR): select an area (Spectacle); Tesseract
//    reads it and the text goes to the clipboard;
//  - "qr TEXT" (or "qr" for the clipboard): a QR code, previewed with Space,
//    copied as an image or saved to Pictures;
//  - "Scan QR Code": select an area; a link in it opens, other text is
//    copied.
class ScreenTools : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;

    // QR code helpers (tested without a screen).
    static QImage makeQr(const QString &text, int scale = 8);
    static QStringList readCodes(const QImage &image);
    // Tesseract's installed languages, without "osd" (script detection).
    static QStringList ocrLanguages();

private:
    // Lets the user select an area of the screen; done gets the image, or a
    // null image when cancelled.
    void captureArea(std::function<void(const QImage &)> done);
    void copyTextFromScreen();
    void scanQr();
};
