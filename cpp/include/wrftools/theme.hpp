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

// Applies dark styling (Fusion style + darkPalette()) when scheme is
// Qt::ColorScheme::Dark, or reverts to whatever style/palette the
// platform's own native theme provided otherwise (Light or Unknown - the
// latter is what an unthemed/offscreen platform reports, so it must not be
// treated as "dark"). The very first call captures the native style name
// and palette (before either is ever touched) as what a later revert goes
// back to, so call this once at startup with the platform's own initial
// QStyleHints::colorScheme() before making any other style/palette change.
// Call again from a QStyleHints::colorSchemeChanged handler to track a
// live OS theme toggle without restarting the app.
void applyColorScheme(QApplication& app, Qt::ColorScheme scheme);

}  // namespace wrftools
