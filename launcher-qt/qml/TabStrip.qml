import QtQuick
import QtQuick.Layouts

// The tabs: pills in a rounded strip; the shown one is yellow. LAUNCHER sits at the right end.
Rectangle {
    id: strip
    property var tabs: []
    readonly property var leftTabs: tabs.filter(t => t.name !== "launcher")
    readonly property var rightTabs: tabs.filter(t => t.name === "launcher")
    implicitHeight: 50
    radius: height / 2
    color: Theme.panel
    border.color: Theme.cardLine

    component TabPill: Rectangle {
        property var tab: ({})
        visible: tab.visible
        width: visible ? label.implicitWidth + 36 : 0
        height: 38
        radius: height / 2
        color: tab.selected ? Theme.gold : (tabMouse.containsMouse ? Theme.field : "transparent")
        Behavior on color { ColorAnimation { duration: 120 } }
        Text {
            id: label
            anchors.centerIn: parent
            text: parent.tab.label
            color: parent.tab.selected ? Theme.ink : (tabMouse.containsMouse ? Theme.text : Theme.muted)
            font.family: Theme.sansSemi; font.pixelSize: Theme.fontBody
        }
        MouseArea {
            id: tabMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: backend.showTab(parent.tab.name)
        }
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        Repeater {
            model: strip.leftTabs
            TabPill { tab: modelData }
        }
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        Repeater {
            model: strip.rightTabs
            TabPill { tab: modelData }
        }
    }
}
