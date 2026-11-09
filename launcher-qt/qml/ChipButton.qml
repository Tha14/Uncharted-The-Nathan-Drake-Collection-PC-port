import QtQuick

// A small on/off pill (the performance counter's items).
Rectangle {
    id: chip
    property string text: ""
    property bool checked: false
    signal toggled(bool value)
    implicitWidth: label.implicitWidth + 26
    implicitHeight: 30
    radius: height / 2
    color: checked ? Theme.gold : (mouse.containsMouse ? Theme.fieldHi : Theme.field)
    border.color: checked ? Theme.gold : Theme.line
    Behavior on color { ColorAnimation { duration: 110 } }
    Text {
        id: label
        anchors.centerIn: parent
        text: chip.text
        color: chip.checked ? Theme.ink : Theme.muted
        font.pixelSize: Theme.fontSmall
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: chip.toggled(!chip.checked)
    }
}
