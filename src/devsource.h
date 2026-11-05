#pragma once

#include "source.h"

// Developer helpers in the main search:
//  - text transforms (see transforms.h): "upper some text", "base64 hello",
//    "json {…}"; a transform's keyword alone applies it to the clipboard;
//  - "count TEXT" (or "count" for the clipboard): words, characters, lines;
//  - generators: "uuid", "password 24", "pin", "random 1-100", "lorem 3",
//    "sha256 TEXT", "timestamp", or a Unix time to read as a date.
// "Transform Clipboard" opens TransformSource, every transform at once.
class DevSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;
};

// The "Transform Clipboard" view: the clipboard's text through every
// transform, the full result in the preview pane.
class TransformSource : public Source
{
    Q_OBJECT
public:
    using Source::Source;
    void query(const QString &text) override;
    QString viewTitle() const override { return tr("Transform"); }
    QString viewPlaceholder() const override { return tr("Filter transforms…"); }
    bool hasPreviewPane() const override { return true; }
};
