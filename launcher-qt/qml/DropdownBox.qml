import QtQuick
import QtQuick.Controls

// A choice from a longer list: the current one in a rounded box, the list in a rounded popup.
Rectangle {
    id: box
    property var options: []   // [[value, label], ...]
    property string value: ""
    property string label: ""
    signal picked(string value)
    implicitWidth: Math.max(180, widest.implicitWidth + 56)
    implicitHeight: 36
    radius: Theme.radiusField
    color: mouse.containsMouse || menu.visible ? Theme.fieldHi : Theme.field
    border.color: menu.visible ? Theme.gold : Theme.line

    Text {   // sizes the box for the longest choice
        id: widest
        visible: false
        font.pixelSize: Theme.fontSmall
        text: box.options.reduce((a, o) => String(o[1]).length > a.length ? String(o[1]) : a, "")
    }
    Text {
        anchors.left: parent.left; anchors.leftMargin: 14
        anchors.right: chevron.left; anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        text: box.label
        color: Theme.text; font.pixelSize: Theme.fontSmall
        elide: Text.ElideRight
    }
    Text {
        id: chevron
        anchors.right: parent.right; anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        text: "▾"
        color: Theme.gold; font.pixelSize: 14
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: menu.open()
    }

    Popup {
        id: menu
        y: box.height + 4
        width: box.width
        height: Math.min(320, list.contentHeight + 12)
        padding: 6
        background: Rectangle { radius: Theme.radiusField; color: Theme.panel; border.color: Theme.line }
        ListView {
            id: list
            anchors.fill: parent
            clip: true
            model: box.options
            delegate: Rectangle {
                width: list.width
                height: 30
                radius: 8
                readonly property bool selected: String(modelData[0]) === String(box.value)
                color: itemMouse.containsMouse ? Theme.gold : (selected ? Theme.fieldHi : "transparent")
                Text {
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    text: (parent.selected ? "✓  " : "     ") + modelData[1]
                    color: itemMouse.containsMouse ? Theme.ink : Theme.text
                    font.pixelSize: Theme.fontSmall
                }
                MouseArea {
                    id: itemMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: { menu.close(); box.picked(String(modelData[0])) }
                }
            }
        }
    }
}
