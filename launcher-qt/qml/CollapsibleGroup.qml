import QtCore
import QtQuick

// A box that folds away to its title: click the title row or the chevron. Open / closed is remembered.
Rectangle {
    id: group
    property var node: ({})
    property bool open: true
    implicitHeight: header.height + body.height + 2
    radius: 14
    color: "#18181d"
    border.color: Theme.cardLine
    clip: true

    Settings {
        id: remembered
        category: "folded"
    }
    // read once the group's title is known; written only when the user folds or opens it
    property bool loaded: false
    onNodeChanged: {
        if (!loaded && node && node.title) {
            loaded = true
            open = String(remembered.value(node.title, "true")) !== "false"
        }
    }
    function toggle() {
        open = !open
        remembered.setValue(node.title || "group", open ? "true" : "false")
    }

    Item {
        id: header
        width: parent.width
        height: 50
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            text: group.node.title || ""
            color: Theme.text; font.family: Theme.sansSemi; font.pixelSize: Theme.fontRowTitle
        }
        Rectangle {   // the chevron, in a round button
            id: chevronButton
            anchors.right: parent.right
            anchors.rightMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            width: 32; height: 32; radius: 16
            color: headerMouse.containsMouse ? Theme.fieldHi : Theme.field
            Behavior on color { ColorAnimation { duration: 120 } }
            Text {
                anchors.centerIn: parent
                text: "▾"
                color: Theme.gold
                font.pixelSize: 16
                rotation: group.open ? 0 : -90
                Behavior on rotation { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
            }
        }
        MouseArea {
            id: headerMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: group.toggle()
        }
    }
    Item {
        id: body
        anchors.top: header.bottom
        width: parent.width
        height: group.open ? content.implicitHeight + 16 : 0
        opacity: group.open ? 1 : 0
        clip: true
        Behavior on height { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: 200 } }
        Column {
            id: content
            x: 18
            width: parent.width - 36
            spacing: 6
            Repeater {
                model: group.node.items
                Loader {
                    width: content.width
                    source: "NodeView.qml"
                    onLoaded: { item.width = Qt.binding(() => width); item.node = Qt.binding(() => modelData) }
                    height: item ? item.implicitHeight : 0
                }
            }
        }
    }
}
