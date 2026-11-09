# Uncharted Launcher: Qt window

The launcher's window in Qt 6 (C++ and QML). Only the look lives here: every setting, button and import step is
still the Python launcher's (`launcher/uncharted_launcher.pyw`).

## How it works

- `launcher/qt_backend.py` builds the Python launcher with its Tk window hidden and talks to this program over
  stdin/stdout (one JSON message per line).
- It sends the window's state: game cards, tabs, status, PLAY, and the shown tab's page as a tree of sections,
  rows and controls.
- A click here is sent back and runs the Tk widget's own code. So a setting added to the Python launcher shows up
  here without any change.
- Message boxes appear as QML dialogs and file pickers as native Qt dialogs. Each one stays open until the Python
  side gets the answer.

## Editing the look

- `qml/Theme.qml`: every colour, radius, size and font.
- `qml/Main.qml`: the window layout (picture, game cards, tabs, page, PLAY bar).
- `qml/PageView.qml`: how a page's sections are placed (two columns from 900 px wide).
- `qml/SectionCard.qml`, `qml/SettingRow.qml`, `qml/NodeView.qml`: a section card, one setting row, and which
  control draws which element.
- The controls: `PillButton`, `ToggleSwitch`, `ChoiceBar`, `DropdownBox`, `ChipButton`, `InputField`, `LogView`,
  `ProgressLine`, `PresetCard`, `MessageDialog`.

Open `CMakeLists.txt` in Qt Creator with the Qt 6 MinGW kit. The QML files also open in Qt Design Studio.

A page's sections come from the Python side: `self.section(page, "Title", "note")` in a page of
`uncharted_launcher.pyw` starts a new card.

## Building (Windows)

```
cmake -S launcher-qt -B build-qt -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/mingw_64 -DCMAKE_BUILD_TYPE=Release
cmake --build build-qt
windeployqt6 --release --qmldir launcher-qt/qml build-qt/UnchartedLauncher.exe
```

Use the Qt MinGW toolchain (`C:/Qt/Tools/mingw1310_64/bin`) on PATH. Build from a path without spaces: the
resource compiler cannot take one.

## Running

The program looks for `launcher/qt_backend.py` in its own folder and up to five folders above, and uses that
folder's `python/` (as the release package ships it). For development:

- `UNCHARTED_LAUNCHER_ROOT`: the launcher folder.
- `UNCHARTED_PYTHON`: the Python to use.
- `UNCHARTED_QT_TAB`: open on this tab.

The Python side's errors go to `analysis/uncharted-user/qt-backend.log`.
