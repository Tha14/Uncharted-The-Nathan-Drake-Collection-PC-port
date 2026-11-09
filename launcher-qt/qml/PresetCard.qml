import QtQuick

// A clickable card drawn by the launcher (Quick Setup's quality levels): its lines of text in a rounded box.
Rectangle {
    id: card
    property var node: ({})
    readonly property var texts: node.texts || []
    implicitWidth: 340
    implicitHeight: column.implicitHeight + 32
    radius: Theme.radiusCard
    color: node.selected ? "#1c1a17" : (mouse.containsMouse ? Theme.fieldHi : Theme.card)
    border.color: node.selected ? Theme.gold : (mouse.containsMouse ? Theme.line : Theme.cardLine)
    border.width: node.selected ? 2 : 1
    Behavior on color { ColorAnimation { duration: 110 } }

    Column {
        id: column
        x: 20; y: 16
        width: card.width - 40
        spacing: 6
        Repeater {
            model: card.texts
            Text {
                width: column.width
                text: modelData.text
                wrapMode: Text.Wrap
                color: Theme.mapColor(modelData.fg)
                font.family: modelData.serif ? Theme.serif : (modelData.bold ? Theme.sansSemi : Theme.sans)
                font.bold: modelData.serif && modelData.bold
                font.pixelSize: Math.round((modelData.size || 9) * 1.333)
            }
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        enabled: !!card.node.click
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: backend.act(card.node.id, "click")
        onDoubleClicked: if (card.node.dclick) backend.act(card.node.id, "dclick")
    }
}
