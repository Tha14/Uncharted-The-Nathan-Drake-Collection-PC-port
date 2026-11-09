import QtQuick

// A rounded progress bar.
Rectangle {
    id: bar
    property real value: 0
    property color track: Theme.field
    property color fill: Theme.gold
    implicitHeight: 8
    radius: height / 2
    color: track
    Rectangle {
        width: Math.max(bar.value > 0 ? bar.height : 0, bar.width * Math.max(0, Math.min(1, bar.value)))
        height: bar.height
        radius: bar.radius
        color: bar.fill
        Behavior on width { NumberAnimation { duration: 200 } }
    }
}
