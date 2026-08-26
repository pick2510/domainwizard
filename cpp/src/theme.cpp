#include "wrftools/theme.hpp"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace wrftools {

QPalette darkPalette() {
    const QColor window(53, 53, 53);
    const QColor base(35, 35, 35);
    const QColor text(220, 220, 220);
    const QColor disabledText(127, 127, 127);
    const QColor highlight(42, 130, 218);

    QPalette palette;
    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, window);
    palette.setColor(QPalette::ToolTipBase, window);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, window);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, highlight);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, Qt::black);
    palette.setColor(QPalette::PlaceholderText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);
    return palette;
}

void applyColorScheme(QApplication& app, Qt::ColorScheme scheme) {
    // Function-local statics: evaluated once, on whichever call happens
    // first - the caller's contract (see theme.hpp) is that this is always
    // the very first call, made before any other style/palette change, so
    // "native" here really does mean untouched.
    static const QString nativeStyleName = app.style() ? app.style()->objectName() : QString();
    static const QPalette nativePalette = app.palette();

    if (scheme == Qt::ColorScheme::Dark) {
        if (auto* fusion = QStyleFactory::create("Fusion")) app.setStyle(fusion);
        app.setPalette(darkPalette());
    } else {
        if (!nativeStyleName.isEmpty()) {
            if (auto* native = QStyleFactory::create(nativeStyleName)) app.setStyle(native);
        }
        app.setPalette(nativePalette);
    }
}

}  // namespace wrftools
