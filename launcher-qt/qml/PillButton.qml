import QtQuick

// A rounded button. kind: "primary" (yellow), "secondary" (dark), "outline" (yellow outline).
Rectangle {
    id: button
    property string text: ""
    property string kind: "secondary"
    property bool big: false
    signal clicked()
    implicitWidth: label.implicitWidth + (big ? 44 : 32)
    implicitHeight: big ? 50 : (kind === "primary" ? 40 : 34)
    radius: height / 2
    color: kind === "primary" ? (mouse.containsMouse ? Theme.goldHi : Theme.gold)
         : kind === "outline" ? (mouse.containsMouse ? Theme.field : "transparent")
         : (mouse.containsMouse ? Theme.fieldHi : Theme.field)
    border.color: kind === "outline" ? Theme.gold : (kind === "primary" ? "transparent" : Theme.line)
    border.width: kind === "outline" ? 1.5 : 1
    scale: mouse.pressed ? 0.97 : 1
    Behavior on color { ColorAnimation { duration: 110 } }
    Behavior on scale { NumberAnimation { duration: 80 } }

    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        color: button.kind === "primary" ? Theme.ink : (button.kind === "outline" ? Theme.gold : Theme.text)
        font.family: Theme.sansSemi
        font.pixelSize: button.big || button.kind === "primary" ? Theme.fontBody : Theme.fontSmall
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
    }
}
