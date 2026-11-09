import QtQuick

// The Collection's picture across the top, fading into the frosted background, with the title on the left and the
// three games on the right. `art` is the picture item (the game cards' glass blurs it).
Item {
    id: hero
    property alias source: art.source
    property alias art: art
    property var games: []
    clip: true

    // the sharp picture, faded out at the bottom
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
    // fades into the frosted background below (the same dark tint, so there is no edge)
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 0.55; color: "transparent" }
            GradientStop { position: 1.0; color: "#d80b0b0d" }
        }
    }

    Column {
        x: Theme.gutter + 8
        y: 34
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

    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.gutter
        anchors.top: parent.top
        anchors.topMargin: 22
        spacing: 12
        Repeater {
            model: hero.games
            GameCard {
                width: 172
                height: 156
                game: modelData
                backdrop: art
            }
        }
    }
}
