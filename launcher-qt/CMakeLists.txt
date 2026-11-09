cmake_minimum_required(VERSION 3.21)
# The launcher's window in Qt 6 (C++ + QML). Open this folder in Qt Creator (or the QML files in Qt Design
# Studio) to edit it. The launcher's logic stays in launcher/uncharted_launcher.pyw: launcher/qt_backend.py runs
# it without its Tk window and this program draws it (see README.md).
project(UnchartedLauncherQt VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Widgets)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(UnchartedLauncher WIN32
    src/main.cpp
    src/backend.cpp
    src/backend.h
    app.rc
)

set(LAUNCHER_QML
    qml/Main.qml
    qml/Theme.qml
    qml/Hero.qml
    qml/GameCard.qml
    qml/TabStrip.qml
    qml/Footer.qml
    qml/PageView.qml
    qml/NodeView.qml
    qml/SectionCard.qml
    qml/SettingRow.qml
    qml/PillButton.qml
    qml/ToggleSwitch.qml
    qml/ChoiceBar.qml
    qml/DropdownBox.qml
    qml/ChipButton.qml
    qml/InputField.qml
    qml/LogView.qml
    qml/ProgressLine.qml
    qml/PresetCard.qml
    qml/MessageDialog.qml
    qml/CollapsibleGroup.qml
)
# Every QML file sits at the module's root at run time (next to its qmldir), so each sees the module's types and
# the Theme singleton.
foreach(file IN LISTS LAUNCHER_QML)
    get_filename_component(name "${file}" NAME)
    set_source_files_properties("${file}" PROPERTIES QT_RESOURCE_ALIAS "${name}")
endforeach()
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(UnchartedLauncher
    URI Launcher
    VERSION 1.0
    QML_FILES ${LAUNCHER_QML}
    RESOURCES assets/uncharted.ico
)

target_link_libraries(UnchartedLauncher PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Widgets)

install(TARGETS UnchartedLauncher RUNTIME DESTINATION .)
