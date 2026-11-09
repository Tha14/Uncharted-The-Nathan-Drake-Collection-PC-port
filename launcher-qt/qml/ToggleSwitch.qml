import QtQuick

// An on/off switch: yellow track when on.
Item {
    id: toggle
    property bool checked: false
    signal toggled(bool value)
    implicitWidth: 52
    implicitHeight: 28

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: toggle.checked ? Theme.gold : Theme.field
        border.color: toggle.checked ? Theme.gold : Theme.line
        Behavior on color { ColorAnimation { duration: 140 } }
        Rectangle {
            width: 22; height: 22; radius: 11
            y: 3
            x: toggle.checked ? parent.width - width - 3 : 3
            color: toggle.checked ? Theme.ink : Theme.muted
            Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: toggle.toggled(!toggle.checked)
    }
}
