import QtQuick
import QtQuick.Layouts

// The bottom bar: status, UPDATE (a newer release) and PLAY / STOP.
// A floating rounded bar, centred under the page.
Rectangle {
    color: Theme.panel
    radius: height / 2
    border.color: Theme.cardLine

    // an update in progress (multiplayer files or the launcher): one line and a fixed-size bar
    Column {
        visible: !!backend.chrome.banner
        anchors.centerIn: parent   // fixed in the middle of the bar, whatever the text and buttons beside it
        spacing: 7
        Text {
            id: updateLine
            anchors.horizontalCenter: parent.horizontalCenter
            text: backend.chrome.banner ? backend.chrome.banner.title : ""
            color: Theme.gold; font.family: Theme.sansSemi; font.pixelSize: Theme.fontSmall
            font.letterSpacing: 0.6
        }
        ProgressLine {
            width: 260
            track: "#38ffffff"
            value: backend.chrome.banner ? backend.chrome.banner.progress : 0
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 32
        anchors.rightMargin: 12
        spacing: 14
        ColumnLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: backend.chrome.banner ? parent.width / 2 - 190 : -1
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
        Item { Layout.fillWidth: true }   // keeps UPDATE and PLAY at the right end
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
