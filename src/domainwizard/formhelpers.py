"""Small form-widget helpers, ported from gis4wrf/plugin/ui/helpers.py.

Only the QGIS-independent pieces are ported here (validated line edits and
grid-layout helpers) - everything else in that module (QgisInterface-based
message bars, layer disposal, etc.) doesn't apply to a standalone app.
"""

from typing import Optional, Union

from PyQt6.QtCore import QLocale
from PyQt6.QtGui import QDoubleValidator, QIntValidator, QValidator
from PyQt6.QtWidgets import QGridLayout, QLabel, QLayout, QLineEdit, QScrollArea, QWidget

DIM_VALIDATOR = QIntValidator()
DIM_VALIDATOR.setBottom(0)
RATIO_VALIDATOR = QIntValidator()
RATIO_VALIDATOR.setBottom(1)


class StringValidator(QValidator):
    def __init__(self, callback):
        super().__init__()
        self.callback = callback

    def validate(self, s, pos):
        if self.callback(s):
            return QValidator.State.Acceptable, s, pos
        else:
            return QValidator.State.Intermediate, s, pos


class MyLineEdit(QLineEdit):
    def __init__(self, required=False) -> None:
        super().__init__()
        self.required = required

    def value(self) -> Union[int, float, str]:
        if isinstance(self.validator(), QDoubleValidator):
            return QLocale().toDouble(self.text())[0]
        elif isinstance(self.validator(), QIntValidator):
            return QLocale().toInt(self.text())[0]
        elif isinstance(self.validator(), StringValidator):
            return self.text()
        else:
            raise NotImplementedError

    def set_value(self, value: Union[int, float, str]) -> None:
        if isinstance(value, str):
            self.setText(value)
        else:
            # QLocale to handle dot vs comma
            self.setText(QLocale().toString(value))

    def is_valid(self) -> bool:
        state = self.validator().validate(self.text(), 0)[0]
        return state == QValidator.State.Acceptable


class WhiteScroll(QScrollArea):
    def __init__(self, widget: QWidget) -> None:
        super().__init__()
        self.setWidgetResizable(True)
        self.setWidget(widget)


def update_input_validation_style(widget: MyLineEdit) -> None:
    """Updates the background color of a line edit.
    Source: https://snorfalorpagus.net/blog/2014/08/09/validating-user-input-in-pyqt4-using-qvalidator/
    """
    green = '#c4df9b'
    yellow = '#fff79a'
    red = '#f6989d'

    required = widget.required

    if widget.is_valid():
        color = green
    elif not widget.text():
        color = yellow if required else ''
    else:
        color = red
    widget.setStyleSheet('QLineEdit { background-color: %s }' % color)


def create_lineedit(validator: QValidator, required: bool = False) -> MyLineEdit:
    """Helper to return a 'validator-ready' line edit."""
    lineedit = MyLineEdit(required)
    lineedit.setValidator(validator)
    lineedit.textChanged.connect(lambda _: update_input_validation_style(lineedit))
    lineedit.textChanged.emit(lineedit.text())
    return lineedit


def add_grid_labeled_widget(grid: QGridLayout, row: int, label_name: str, widget: Union[QWidget, QLayout]) -> None:
    grid.addWidget(QLabel(label_name + ':'), row, 0)
    if isinstance(widget, QWidget):
        grid.addWidget(widget, row, 1)
    else:
        grid.addLayout(widget, row, 1)


def add_grid_lineedit(grid: QGridLayout, row: int, label_name: str, validator: Optional[QValidator],
                       unit: Optional[str] = None, required: bool = False) -> MyLineEdit:
    """Helper to return a 'validator-ready' grid layout
    composed of a name label, line edit and optional unit.
    """
    lineedit = create_lineedit(validator, required)
    add_grid_labeled_widget(grid, row, label_name, lineedit)
    if unit:
        grid.addWidget(QLabel(unit), row, 2)
    return lineedit
