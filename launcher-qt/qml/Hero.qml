import QtQuick

// The Collection's picture across the top, fading into the window's black, with the title.
Item {
    id: hero
    property alias source: art.source
    clip: true

    Rectangle { anchors.fill: parent; color: "#d9c9a6"; visible: art.status !== Image.Ready }
    Image {
        id: art
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: -width * 0.0113   // as the Tk launcher: 2 % down the picture
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
        mipmap: true
    }
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 0.55; color: "transparent" }
            GradientStop { position: 1.0; color: Theme.bg }
        }
    }
    Column {
        x: Theme.gutter + 8
        y: 26
        spacing: 2
        Text {
            text: "UNCHARTED"
            color: "#3b2914"; font.family: Theme.serif; font.pixelSize: 52; font.bold: true
        }
        Text {
            x: 3
            text: "T H E   N A T H A N   D R A K E   C O L L E C T I O N"
            color: "#8a6326"; font.family: Theme.sansSemi; font.pixelSize: 14
        }
        Rectangle { x: 3; width: 330; height: 2; color: "#b98d45"; radius: 1 }
    }
}
