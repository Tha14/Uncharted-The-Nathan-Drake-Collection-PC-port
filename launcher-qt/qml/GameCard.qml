import QtQuick

// One game, on frosted glass over the cover art: click selects it, double-click plays it.
Item {
    id: card
    property var game: ({})
    property Item backdrop: null
    readonly property bool selected: !!game.selected
    scale: mouse.pressed ? 0.98 : 1
    Behavior on scale { NumberAnimation { duration: 90 } }

    GlassPanel {
        anchors.fill: parent
        backdrop: card.backdrop
        radius: 18
        tint: card.selected ? "#b81a160f" : (mouse.containsMouse ? "#a01d1d22" : "#8c121216")
    }
    Rectangle {   // outline: yellow when selected
        anchors.fill: parent
        radius: 18
        color: "transparent"
        border.color: card.selected ? Theme.gold : (mouse.containsMouse ? "#60ffffff" : "#30ffffff")
        border.width: card.selected ? 2 : 1
        Behavior on border.color { ColorAnimation { duration: 140 } }
    }
    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 16
        anchors.rightMargin: 14
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4
        Text {
            text: card.game.number || ""
            color: card.selected ? Theme.gold : "#c8c3b8"
            font.family: Theme.serif; font.pixelSize: 40; font.bold: true
        }
        Text {
            width: parent.width
            text: card.game.title || ""
            color: Theme.text; font.family: Theme.serif; font.pixelSize: 15; font.bold: true
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            text: (card.game.subtitle || "").split("•")[0].trim()
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
