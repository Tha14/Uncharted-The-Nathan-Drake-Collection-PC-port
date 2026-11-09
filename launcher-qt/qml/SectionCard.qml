import QtQuick

// A rounded card holding one section of a page: a heading with a yellow mark, a note, then its rows.
Rectangle {
    id: card
    property var node: ({})
    implicitHeight: body.implicitHeight + 40
    radius: Theme.radiusCard
    color: Theme.card
    border.color: Theme.cardLine

    Column {
        id: body
        x: 22; y: 20
        width: card.width - 44
        spacing: 8
        Row {
            visible: !!card.node.title
            spacing: 10
            Rectangle { width: 4; height: 18; radius: 2; color: Theme.gold; anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: card.node.title || ""
                color: Theme.text; font.family: Theme.sansSemi; font.pixelSize: Theme.fontSection
            }
        }
        Text {
            visible: !!card.node.note
            width: parent.width
            text: card.node.note || ""
            color: Theme.muted; font.pixelSize: Theme.fontSmall; wrapMode: Text.Wrap
        }
        Item { width: 1; height: 2; visible: !!card.node.title }
        Repeater {
            model: card.node.items
            Loader {   // NodeView, loaded by URL (a type cannot contain the type that contains it)
                width: body.width
                source: "NodeView.qml"
                onLoaded: { item.width = Qt.binding(() => width); item.node = Qt.binding(() => modelData) }
                height: item ? item.implicitHeight : 0
            }
        }
    }
}
