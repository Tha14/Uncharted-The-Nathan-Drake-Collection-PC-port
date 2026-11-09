import QtQuick

// One game: click selects it, double-click plays it.
Rectangle {
    id: card
    property var game: ({})
    readonly property bool selected: !!game.selected
    radius: Theme.radiusCard
    color: selected ? "#1c1a17" : (mouse.containsMouse ? Theme.fieldHi : Theme.card)
    border.color: selected ? Theme.gold : (mouse.containsMouse ? Theme.line : Theme.cardLine)
    border.width: selected ? 2 : 1
    Behavior on color { ColorAnimation { duration: 120 } }

    Text {
        id: number
        anchors.left: parent.left
        anchors.leftMargin: 22
        anchors.verticalCenter: parent.verticalCenter
        text: card.game.number || ""
        color: card.selected ? Theme.gold : Theme.dim
        font.family: Theme.serif; font.pixelSize: 38; font.bold: true
    }
    Column {
        anchors.left: number.right
        anchors.leftMargin: 18
        anchors.right: parent.right
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 3
        Text {
            width: parent.width
            text: card.game.title || ""
            color: Theme.text; font.family: Theme.serif; font.pixelSize: 16; font.bold: true
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            text: card.game.subtitle || ""
            color: Theme.muted; font.pixelSize: Theme.fontSmall
            elide: Text.ElideRight
        }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: backend.selectGame(card.game.key)
        onDoubleClicked: backend.playGame(card.game.key)
    }
}
