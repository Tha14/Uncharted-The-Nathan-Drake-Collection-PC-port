import QtQuick
import QtQuick.Layouts

// The bottom bar: status, UPDATE (a newer release) and PLAY / STOP.
Rectangle {
    color: Theme.panel
    Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.cardLine }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.gutter
        anchors.rightMargin: Theme.gutter
        spacing: 14
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                Layout.fillWidth: true
                text: backend.chrome.status ? backend.chrome.status.text : ""
                color: Theme.text; font.family: Theme.sansSemi; font.pixelSize: Theme.fontRowTitle
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: backend.chrome.status ? backend.chrome.status.detail : ""
                color: Theme.muted; font.pixelSize: Theme.fontSmall
                elide: Text.ElideRight
            }
        }
        PillButton {
            visible: !!backend.chrome.update
            text: backend.chrome.update || ""
            kind: "outline"
            big: true
            onClicked: backend.installUpdate()
        }
        Rectangle {
            id: play
            readonly property string label: backend.chrome.play || "PLAY"
            readonly property bool blocked: label === "UPDATING..."
            readonly property bool running: label === "STOP"
            Layout.preferredWidth: 220
            Layout.preferredHeight: 56
            radius: height / 2
            color: blocked ? Theme.field : (running ? Theme.red : (playMouse.containsMouse ? Theme.goldHi : Theme.gold))
            border.color: blocked ? Theme.line : "transparent"
            Behavior on color { ColorAnimation { duration: 120 } }
            Text {
                anchors.centerIn: parent
                text: play.label
                color: play.blocked ? Theme.muted : (play.running ? Theme.text : Theme.ink)
                font.family: Theme.serif; font.pixelSize: 22; font.bold: true
            }
            MouseArea {
                id: playMouse
                anchors.fill: parent
                hoverEnabled: true
                enabled: !play.blocked
                cursorShape: play.blocked ? Qt.ArrowCursor : Qt.PointingHandCursor
                onClicked: backend.play()
            }
        }
    }
}
