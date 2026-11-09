import QtQuick
import QtQuick.Controls

// A log (the import's last lines): monospace, scrolled to the end.
Rectangle {
    id: log
    property string text: ""
    implicitHeight: 170
    radius: Theme.radiusField
    color: "#101013"
    border.color: Theme.line
    clip: true

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: 10
        contentWidth: body.implicitWidth
        contentHeight: body.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { width: 8 }
        Text {
            id: body
            text: log.text
            color: Theme.muted
            font.family: "Consolas"
            font.pixelSize: 11
            textFormat: Text.PlainText
        }
        onContentHeightChanged: contentY = Math.max(0, contentHeight - height)
    }
}
