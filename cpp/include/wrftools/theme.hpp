#pragma once

#include <Qt>

class QApplication;
class QPalette;

namespace wrftools {

// A Fusion-style dark palette (the standard recipe used across many Qt6
// desktop apps) - not built from arbitrary guesses: WindowText/Text/
// ButtonText stay a light gray rather than pure white, and disabled
// entries stay a distinct mid-gray, which is what makes a manually-built
// dark QPalette read as properly dark instead of every widget blending
// into the same background shade.
[[nodiscard]] QPalette darkPalette();

// Overrides `reported` (normally QStyleHints::colorScheme()'s own answer)
// with GNOME's own `gsettings org.gnome.desktop.interface color-scheme`
// preference, whenever that's reachable. QStyleHints::colorScheme()
// depends on desktop-integration plumbing - a QPA platform theme plugin,
// or a reachable xdg-desktop-portal D-Bus service reporting
// org.freedesktop.appearance's color-scheme - that not every GNOME/Qt
// combination has wired up: colorScheme() staying Light/Unknown while the
// desktop is actually in dark mode is exactly that gap, not a
// hypothetical. `gsettings` reads GNOME's own setting directly, with no
// such dependency, and is present on essentially every GNOME desktop.
// Falls back to `reported` unchanged on every other platform/desktop
// (macOS, Windows, non-GNOME Linux, or a GNOME desktop with no
// `gsettings` binary). Call this on whatever QStyleHints::colorScheme()
// reports before passing the result to applyColorScheme() below.
[[nodiscard]] Qt::ColorScheme resolveColorScheme(Qt::ColorScheme reported);

// Applies dark styling (Fusion style + darkPalette()) when scheme is
// Qt::ColorScheme::Dark, or reverts to whatever style/palette the
// platform's own native theme provided otherwise (Light or Unknown - the
// latter is what an unthemed/offscreen platform reports, so it must not be
// treated as "dark"). The very first call captures the native style name
// and palette (before either is ever touched) as what a later revert goes
// back to, so call this once at startup with resolveColorScheme() applied
// to the platform's own initial QStyleHints::colorScheme(), before making
// any other style/palette change. Call again from a QStyleHints::
// colorSchemeChanged handler (through resolveColorScheme() again) to
// track a live OS theme toggle without restarting the app - though on a
// GNOME desktop where colorScheme() itself never fires that signal, only
// another explicit call (e.g. the next app launch) will pick up a
// mid-session toggle.
void applyColorScheme(QApplication& app, Qt::ColorScheme scheme);

}  // namespace wrftools
