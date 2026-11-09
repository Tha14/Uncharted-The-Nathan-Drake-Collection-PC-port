import QtQuick

// The Collection's picture across the top, fading to transparent into the frosted background (no edge), with the
// title on the left and the three games on the right. `art` is the picture item (the game cards' glass blurs it).
Item {
    id: hero
    property alias source: art.source
    property alias art: art
    property var games: []
    clip: true

    // the picture the game cards' glass samples; not drawn itself (its clipped holder has no height)
    Item {
        width: hero.width
        height: 0
        clip: true
        Image {
            id: art
            width: hero.width
            y: -width * 0.0113   // as the Tk launcher: 2 % down the picture
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            mipmap: true
        }
    }

    // what is drawn: the picture, erased along a smooth vertical gradient (solid at the top, fully transparent at
    // the bottom), so it melts into the blurred background behind the window with no edge
    Canvas {
        id: picture
        anchors.fill: parent
        canvasSize: Qt.size(Math.max(1, width), Math.max(1, height))
        canvasWindow: Qt.rect(0, 0, Math.max(1, width), Math.max(1, height))
        tileSize: canvasSize
        renderStrategy: Canvas.Immediate
        readonly property string url: String(art.source)
        onUrlChanged: if (url) loadImage(url)
        onImageLoaded: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            if (!url || !isImageLoaded(url) || art.paintedHeight <= 0)
                return
            ctx.drawImage(url, 0, art.y, width, art.paintedHeight)
            ctx.globalCompositeOperation = "destination-out"
            // fully erased a little above the bottom edge (no last row of the picture left as a line)
            const fade = ctx.createLinearGradient(0, height * 0.40, 0, height * 0.94)
            fade.addColorStop(0.0, "rgba(0,0,0,0)")
            fade.addColorStop(0.55, "rgba(0,0,0,0.6)")
            fade.addColorStop(1.0, "rgba(0,0,0,1)")
            ctx.fillStyle = fade
            ctx.fillRect(-2, -2, width + 4, height + 4)
        }
        Connections {
            target: art
            function onPaintedHeightChanged() { picture.requestPaint() }
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
        anchors.topMargin: 42   // level with the top of the UNCHARTED letters
        spacing: 12
        Repeater {
            model: hero.games
            GameCard {
                width: 172
                height: 92
                game: modelData
                backdrop: art
            }
        }
    }
}
