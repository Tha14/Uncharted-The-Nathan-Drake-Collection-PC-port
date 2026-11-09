import QtQuick

// A row of choices in a rounded track; the chosen one is a yellow pill.
Rectangle {
    id: bar
    property var options: []   // [[value, label], ...]
    property string value: ""
    property bool active: true
    signal picked(string value)
    implicitWidth: row.implicitWidth + 8
    implicitHeight: 36
    radius: height / 2
    color: Theme.field
    border.color: Theme.line

    Row {
        id: row
        x: 4
        anchors.verticalCenter: parent.verticalCenter
        Repeater {
            model: bar.options
            Rectangle {
                readonly property bool selected: String(modelData[0]) === String(bar.value)
                width: optionText.implicitWidth + 26
                height: 28
                radius: height / 2
                color: selected ? (bar.active ? Theme.gold : Theme.fieldHi)
                                : (optionMouse.containsMouse && bar.active ? Theme.fieldHi : "transparent")
                Behavior on color { ColorAnimation { duration: 110 } }
                Text {
                    id: optionText
                    anchors.centerIn: parent
                    text: modelData[1]
                    color: !bar.active ? Theme.muted : (parent.selected ? Theme.ink : Theme.text)
                    font.family: parent.selected ? Theme.sansSemi : Theme.sans
                    font.pixelSize: Theme.fontSmall
                }
                MouseArea {
                    id: optionMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: bar.active
                    cursorShape: Qt.PointingHandCursor
                    onClicked: bar.picked(String(modelData[0]))
                }
            }
        }
    }
}
