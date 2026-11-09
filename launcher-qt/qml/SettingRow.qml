import QtQuick

// One setting: its name and explanation, and its control on the right, or under the text when the control is
// too wide to sit beside it (a long row of choices in a narrow card).
Item {
    id: row
    property var node: ({})
    readonly property real controlWidth: control.item ? control.item.naturalWidth : 0
    readonly property bool stacked: controlWidth > row.width * 0.46
    implicitHeight: stacked ? texts.implicitHeight + 10 + control.height + 8
                            : Math.max(texts.implicitHeight, control.height) + 10

    Column {
        id: texts
        anchors.left: parent.left
        anchors.right: row.stacked ? parent.right : control.left
        anchors.rightMargin: row.stacked ? 0 : 20
        y: row.stacked ? 4 : (row.implicitHeight - implicitHeight) / 2
        spacing: 3
        Text {
            width: parent.width
            text: row.node.title || ""
            color: Theme.text; font.family: Theme.sansSemi; font.pixelSize: Theme.fontRowTitle
            wrapMode: Text.Wrap
        }
        Text {
            visible: !!row.node.hint
            width: parent.width
            text: row.node.hint || ""
            color: Theme.muted; font.pixelSize: Theme.fontSmall
            wrapMode: Text.Wrap
            lineHeight: 1.15
        }
    }
    Loader {   // NodeView, loaded by URL (a type cannot contain the type that contains it)
        id: control
        source: "NodeView.qml"
        x: row.stacked ? 0 : row.width - width
        y: row.stacked ? texts.implicitHeight + 14 : (row.implicitHeight - height) / 2
        width: Math.min(row.controlWidth, row.width)
        height: item ? item.implicitHeight : 0
        onLoaded: { item.node = Qt.binding(() => row.node.control || ({})); item.width = Qt.binding(() => width) }
    }
}
