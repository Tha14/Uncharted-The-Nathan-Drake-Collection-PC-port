import QtQuick
import QtQuick.Layouts

// The tabs: pills in a rounded strip; the shown one is yellow.
Rectangle {
    id: strip
    property var tabs: []
    implicitHeight: 50
    radius: height / 2
    color: Theme.panel
    border.color: Theme.cardLine

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        Repeater {
            model: strip.tabs
            Rectangle {
                visible: modelData.visible
                width: visible ? label.implicitWidth + 36 : 0
                height: 38
                radius: height / 2
                color: modelData.selected ? Theme.gold : (tabMouse.containsMouse ? Theme.field : "transparent")
                Behavior on color { ColorAnimation { duration: 120 } }
                Text {
                    id: label
                    anchors.centerIn: parent
                    text: modelData.label
                    color: modelData.selected ? Theme.ink : (tabMouse.containsMouse ? Theme.text : Theme.muted)
                    font.family: Theme.sansSemi; font.pixelSize: Theme.fontBody
                }
                MouseArea {
                    id: tabMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: backend.showTab(modelData.name)
                }
            }
        }
    }
}
