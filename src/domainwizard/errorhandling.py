"""Global exception handling for slots/callbacks invoked by Qt.

PyQt6's default behavior for an exception raised inside a Qt slot (e.g. a
button's `clicked` handler) is to abort the whole process (SIGABRT) rather
than just letting the exception propagate to Python's normal error
reporting - `sys.excepthook` is never even consulted for exceptions raised
across the Qt/C++ call boundary. install() replaces PyQt6's own hook
(`QtCore.qInstallMessageHandler` doesn't cover this; it's specifically
`sys.excepthook` that PyQt6 checks before deciding to abort) with one that
shows a dialog and lets the application keep running instead.
"""

import sys
import traceback

from PyQt6.QtWidgets import QMessageBox

from gis4wrf.core import UserError


def install() -> None:
    sys.excepthook = _handle_exception


def _handle_exception(exc_type, exc_value, exc_tb) -> None:
    if issubclass(exc_type, UserError):
        QMessageBox.warning(None, 'Domain Wizard', str(exc_value))
        return

    details = ''.join(traceback.format_exception(exc_type, exc_value, exc_tb))
    print(details, file=sys.stderr)
    box = QMessageBox(QMessageBox.Icon.Critical, 'Domain Wizard - Unexpected Error',
                       f'{exc_type.__name__}: {exc_value}')
    box.setDetailedText(details)
    box.exec()
