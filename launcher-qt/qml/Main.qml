import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

// The launcher window: picture, game cards, tabs, the shown tab's page, and the PLAY bar.
ApplicationWindow {
    id: window
    width: Theme.windowWidth
    height: Math.min(940, Screen.desktopAvailableHeight - 40)
    minimumWidth: 980
    minimumHeight: 640
    visible: false   // shown by restoreGeometry(), at the last size and place
    color: Theme.bg
    title: backend.chrome.title || "Uncharted: The Nathan Drake Collection"

    // The window opens where and as large as it was closed (and maximized if it was), if that is still on a screen.
    Settings {
        id: saved
        category: "window"
        property int x: -1
        property int y: -1
        property int width: 0
        property int height: 0
        property bool maximized: false
    }
    function saveGeometry() {
        saved.maximized = window.visibility === Window.Maximized
        if (!saved.maximized) {
            saved.x = window.x; saved.y = window.y; saved.width = window.width; saved.height = window.height
        }
        saved.sync()
    }
    Component.onCompleted: {
        if (saved.width >= minimumWidth && saved.height >= minimumHeight) {
            width = saved.width
            height = saved.height
        }
        const sx = saved.x, sy = saved.y
        if (sx >= Screen.virtualX - 50 && sx <= Screen.virtualX + Screen.desktopAvailableWidth - 200
                && sy >= Screen.virtualY && sy <= Screen.virtualY + Screen.desktopAvailableHeight - 200) {
            x = sx
            y = sy
        }
        if (saved.maximized)
            showMaximized()
        else
            show()
    }

    property bool closing: false
    onClosing: (close) => {
        saveGeometry()
        if (!closing && backend.ready) {
            close.accepted = false
            closing = true
            backend.close()
        }
    }

    // Frosted background: the cover art across the whole window, blurred and darkened, so text on it stays readable.
    Image {
        id: backgroundArt
        anchors.fill: parent
        source: backend.fileUrl(backend.chrome.hero || "")
        fillMode: Image.PreserveAspectCrop
        verticalAlignment: Image.AlignTop
        asynchronous: true
        visible: false
    }
    MultiEffect {
        anchors.fill: parent
        source: backgroundArt
        blurEnabled: true
        blurMax: 64
        blur: 0.85
        saturation: -0.2
        visible: backgroundArt.status === Image.Ready
    }
    Rectangle { anchors.fill: parent; color: "#9a0b0b0d" }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // the picture, with the title and the three games on it
        Hero {
            Layout.fillWidth: true
            Layout.preferredHeight: 210
            source: backend.fileUrl(backend.chrome.hero || "")
            games: backend.chrome.games || []
        }

        // Uncharted 3 multiplayer update in progress (the Tk launcher's gold banner)
        Rectangle {
            visible: !!backend.chrome.banner
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gutter
            Layout.rightMargin: Theme.gutter
            Layout.topMargin: -6
            Layout.bottomMargin: 6
            Layout.preferredHeight: bannerColumn.implicitHeight + 24
            radius: Theme.radiusCard
            color: Theme.gold
            ColumnLayout {
                id: bannerColumn
                anchors.fill: parent
                anchors.margins: 12
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                spacing: 4
                Text {
                    text: backend.chrome.banner ? backend.chrome.banner.title : ""
                    color: Theme.ink; font.family: Theme.sansSemi; font.pixelSize: Theme.fontBody
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                }
                Text {
                    text: backend.chrome.banner ? backend.chrome.banner.detail : ""
                    color: Theme.ink; font.pixelSize: Theme.fontSmall
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                }
                ProgressLine {
                    Layout.fillWidth: true
                    value: backend.chrome.banner ? backend.chrome.banner.progress : 0
                    track: Qt.darker(Theme.gold, 1.25)
                    fill: Theme.ink
                }
            }
        }

        TabStrip {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.gutter
            Layout.rightMargin: Theme.gutter
            Layout.topMargin: 0
            tabs: backend.chrome.tabs || []
        }

        PageView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 14
            items: backend.page
            tab: backend.tab
        }

        Footer {   // floating: centred, as wide as the tabs and the page, with a gap under it
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: window.width - 2 * Theme.gutter
            Layout.preferredHeight: 76
            Layout.topMargin: 10
            Layout.bottomMargin: 16
        }
    }

    // starting: the Python part builds the launcher (a second or two)
    Rectangle {
        anchors.fill: parent
        visible: !backend.ready
        color: Theme.bg
        Column {
            anchors.centerIn: parent
            spacing: 14
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "UNCHARTED"
                color: Theme.gold; font.family: Theme.serif; font.pixelSize: 44; font.bold: true
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: backend.error || "Starting..."
                color: backend.error ? Theme.red : Theme.muted; font.pixelSize: Theme.fontBody
            }
        }
    }

    // A Controls slot waits for its key: the next key, mouse button or wheel step goes to the launcher (as the Tk
    // window's capture did). The layer covers the window only while a slot waits.
    Item {
        id: captureLayer
        anchors.fill: parent
        visible: !!backend.chrome.capturing
        focus: visible
        onVisibleChanged: if (visible) forceActiveFocus()
        z: 50

        // Qt key -> the Tk keysym the launcher's capture_key expects
        function keysym(event) {
            const k = event.key
            const map = {
                [Qt.Key_Space]: "space", [Qt.Key_Return]: "Return", [Qt.Key_Enter]: "KP_Enter",
                [Qt.Key_Escape]: "Escape", [Qt.Key_Tab]: "Tab", [Qt.Key_Backspace]: "BackSpace",
                [Qt.Key_Up]: "Up", [Qt.Key_Down]: "Down", [Qt.Key_Left]: "Left", [Qt.Key_Right]: "Right",
                [Qt.Key_Insert]: "Insert", [Qt.Key_Delete]: "Delete", [Qt.Key_Home]: "Home", [Qt.Key_End]: "End",
                [Qt.Key_PageUp]: "Prior", [Qt.Key_PageDown]: "Next", [Qt.Key_CapsLock]: "Caps_Lock",
                [Qt.Key_Comma]: "comma", [Qt.Key_Period]: "period", [Qt.Key_Slash]: "slash",
                [Qt.Key_Semicolon]: "semicolon", [Qt.Key_Apostrophe]: "apostrophe", [Qt.Key_BracketLeft]: "bracketleft",
                [Qt.Key_BracketRight]: "bracketright", [Qt.Key_Backslash]: "backslash", [Qt.Key_Minus]: "minus",
                [Qt.Key_Equal]: "equal", [Qt.Key_QuoteLeft]: "grave"
            }
            if (k === Qt.Key_Shift) return event.nativeScanCode === 0x36 ? "Shift_R" : "Shift_L"
            if (k === Qt.Key_Control) return event.nativeScanCode === 0x11d ? "Control_R" : "Control_L"
            if (k === Qt.Key_Alt) return event.nativeScanCode === 0x138 ? "Alt_R" : "Alt_L"
            if (k >= Qt.Key_F1 && k <= Qt.Key_F24) return "F" + (k - Qt.Key_F1 + 1)
            if (k >= Qt.Key_A && k <= Qt.Key_Z) return String.fromCharCode(k).toLowerCase()
            if (k >= Qt.Key_0 && k <= Qt.Key_9) return String.fromCharCode(k)
            return map[k] || event.text || ""
        }
        Keys.onPressed: (event) => {
            event.accepted = true
            if (!event.isAutoRepeat) backend.capture(keysym(event), "")
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: (mouse) => {
                const names = { [Qt.LeftButton]: "left", [Qt.RightButton]: "right", [Qt.MiddleButton]: "middle",
                                [Qt.BackButton]: "x1", [Qt.ForwardButton]: "x2" }
                if (names[mouse.button]) backend.capture("", names[mouse.button])
            }
            onWheel: (wheel) => backend.capture("", wheel.angleDelta.y > 0 ? "wheel_up" : "wheel_down")
        }
    }

    MessageDialog {
        id: messageDialog
        anchors.centerIn: parent
    }
    Connections {
        target: backend
        function onMessageRequested(dialog, kind, title, message, detail) {
            messageDialog.ask(dialog, kind, title, message, detail)
        }
    }
}
