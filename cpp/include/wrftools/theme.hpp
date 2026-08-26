#pragma once

class QApplication;
class QPalette;
class QSettings;

namespace wrftools {

// Mirrors Qt::ColorScheme (Light/Dark/Unknown) without depending on it -
// Qt::ColorScheme and QStyleHints::colorScheme()/colorSchemeChanged were
// only added in Qt 6.5, and this project's CMake minimum stays at 6.4 (the
// version Ubuntu 24.04's packaged Qt6 actually ships, which the Linux CI
// job builds against) precisely so this header has no hard Qt-6.5
// dependency. main_window.cpp maps the real Qt::ColorScheme to this one
// behind a `#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)` guard, so
// System-preference live OS-theme-tracking is available when built
// against 6.5+ and simply absent (not a compile error) otherwise.
enum class ColorScheme { Light, Dark, Unknown };

// The user's explicit choice from Options > Theme, persisted via QSettings
// (see themePreference()/setThemePreference() below) - System means "keep
// following the OS", overriding automatic detection entirely for Light/
// Dark. Distinct from ColorScheme, which only ever means Light/Dark/
// Unknown and has no "explicitly follow the OS" concept of its own.
enum class ThemePreference { System, Light, Dark };

// Reads the persisted Options > Theme choice, defaulting to System when
// nothing has been saved yet (a fresh install) or the stored value isn't
// recognized. Takes `settings` by reference rather than constructing its
// own QSettings() so tests can point it at an isolated (e.g. QTemporaryDir
// IniFormat) instance instead of the real per-user config file; real
// callers pass a default-constructed QSettings() (using the organization/
// application name QApplication::setOrganizationName/setApplicationName
// already set in main.cpp).
[[nodiscard]] ThemePreference themePreference(const QSettings& settings);
void setThemePreference(QSettings& settings, ThemePreference preference);

// A Fusion-style dark palette (the standard recipe used across many Qt6
// desktop apps) - not built from arbitrary guesses: WindowText/Text/
// ButtonText stay a light gray rather than pure white, and disabled
// entries stay a distinct mid-gray, which is what makes a manually-built
// dark QPalette read as properly dark instead of every widget blending
// into the same background shade.
[[nodiscard]] QPalette darkPalette();

// Overrides `reported` (normally the platform/Qt's own answer, mapped to
// ColorScheme - see main_window.cpp) with GNOME's own `gsettings
// org.gnome.desktop.interface color-scheme` preference, whenever that's
// reachable. Qt's own OS-scheme detection depends on desktop-integration
// plumbing - a QPA platform theme plugin, or a reachable
// xdg-desktop-portal D-Bus service reporting org.freedesktop.appearance's
// color-scheme - that not every GNOME/Qt combination has wired up:
// reporting Light/Unknown while the desktop is actually in dark mode is
// exactly that gap, not a hypothetical. `gsettings` reads GNOME's own
// setting directly, with no such dependency, and is present on
// essentially every GNOME desktop. Falls back to `reported` unchanged on
// every other platform/desktop (macOS, Windows, non-GNOME Linux, or a
// GNOME desktop with no `gsettings` binary, or where even GNOME's own
// gsettings has no reliable signal - see applyColorScheme's ThemePreference
// note below for what covers that last case).
[[nodiscard]] ColorScheme resolveColorScheme(ColorScheme reported);

// Applies dark styling (Fusion style + darkPalette()) when scheme is
// ColorScheme::Dark, or reverts to whatever style/palette the platform's
// own native theme provided otherwise (Light or Unknown - the latter is
// what an unthemed/offscreen platform, or a Qt built without 6.5's
// QStyleHints::colorScheme(), effectively reports, so it must not be
// treated as "dark"). The very first call captures the native style name
// and palette (before either is ever touched) as what a later revert goes
// back to, so call this once at startup - typically with
// resolveColorScheme() applied to the platform's own initial reported
// scheme - before making any other style/palette change. Automatic
// detection isn't always enough on its own (a real GNOME desktop was
// found with an explicit dark mode set where neither Qt's own detection
// nor GNOME's gsettings gave any usable signal); MainWindow's Options >
// Theme menu is the resulting manual override - Light/Dark bypass
// resolveColorScheme() entirely and call this directly with a fixed
// scheme that then sticks regardless of what the OS reports afterward.
void applyColorScheme(QApplication& app, ColorScheme scheme);

}  // namespace wrftools
