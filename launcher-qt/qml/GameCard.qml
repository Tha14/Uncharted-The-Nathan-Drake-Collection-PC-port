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
        spacing: 5
        Text {   // "Uncharted 1" (the subtitle's first part), where the big number was
            width: parent.width
            text: (card.game.subtitle || "").split("•")[0].trim()
            color: card.selected ? Theme.gold : "#d6d1c6"
            font.family: Theme.sansSemi; font.pixelSize: 16
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            text: card.game.title || ""
            color: Theme.text; font.family: Theme.serif; font.pixelSize: 15; font.bold: true
            wrapMode: Text.Wrap
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
