#include "wrftools/theme.hpp"

#include <QApplication>
#include <QPalette>
#include <QProcess>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>

#include <optional>

namespace wrftools {
namespace {
constexpr const char* kThemePreferenceKey = "theme/mode";
}  // namespace

ThemePreference themePreference(const QSettings& settings) {
    const auto value = settings.value(kThemePreferenceKey).toString();
    if (value == "light") return ThemePreference::Light;
    if (value == "dark") return ThemePreference::Dark;
    return ThemePreference::System;  // default for a fresh install or an unrecognized value
}

void setThemePreference(QSettings& settings, ThemePreference preference) {
    switch (preference) {
        case ThemePreference::Light: settings.setValue(kThemePreferenceKey, "light"); break;
        case ThemePreference::Dark: settings.setValue(kThemePreferenceKey, "dark"); break;
        case ThemePreference::System: settings.setValue(kThemePreferenceKey, "system"); break;
    }
}

namespace {
// GNOME's own preference, queried directly via `gsettings`, rather than
// trusting QStyleHints::colorScheme() - that relies on either a QPA
// platform theme plugin or a reachable xdg-desktop-portal D-Bus service
// reporting org.freedesktop.appearance's color-scheme, and plenty of real
// GNOME/Qt combinations have neither wired up (colorScheme() staying
// Light/Unknown while the desktop is actually in dark mode is exactly
// that gap, not a hypothetical). `gsettings` reads GNOME's own setting
// with no such dependency and is present on essentially every GNOME
// desktop; std::nullopt (not present, timed out, or an unrecognized
// value) means "no opinion", not "light".
std::optional<Qt::ColorScheme> gnomeColorScheme() {
    QProcess process;
    process.start("gsettings", {"get", "org.gnome.desktop.interface", "color-scheme"});
    if (!process.waitForFinished(500) || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) return std::nullopt;
    const auto output = QString::fromUtf8(process.readAllStandardOutput());
    if (output.contains("prefer-dark", Qt::CaseInsensitive)) return Qt::ColorScheme::Dark;
    if (output.contains("prefer-light", Qt::CaseInsensitive) || output.contains("default", Qt::CaseInsensitive)) return Qt::ColorScheme::Light;
    return std::nullopt;
}
}  // namespace

Qt::ColorScheme resolveColorScheme(Qt::ColorScheme reported) {
    if (const auto gnome = gnomeColorScheme()) return *gnome;
    return reported;
}

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
    // scheme is expected to already be resolveColorScheme()'s output, not
    // a raw QStyleHints::colorScheme() report - see theme.hpp. This
    // function only decides style/palette from it.
    //
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
