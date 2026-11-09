import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The launcher's message boxes (information, warnings, questions) as a rounded dark dialog.
Popup {
    id: dialog
    property int dialogId: 0
    property string kind: "info"
    property string title: ""
    property string message: ""
    property string detail: ""
    property var queue: []
    modal: true
    closePolicy: Popup.NoAutoClose
    width: Math.min(560, parent ? parent.width - 80 : 560)
    padding: 26
    background: Rectangle { radius: 20; color: Theme.panel; border.color: Theme.line }
    Overlay.modal: Rectangle { color: "#a0000000" }

    function ask(id, kind, title, message, detail) {
        if (visible) { queue.push([id, kind, title, message, detail]); return }
        dialogId = id; dialog.kind = kind; dialog.title = title; dialog.message = message; dialog.detail = detail
        open()
    }
    function reply(value) {
        close()
        backend.answer(dialogId, value)
        if (queue.length) { const q = queue.shift(); ask(q[0], q[1], q[2], q[3], q[4]) }
    }
    readonly property bool question: kind !== "info" && kind !== "warning" && kind !== "error"

    contentItem: ColumnLayout {
        spacing: 14
        RowLayout {
            spacing: 12
            Rectangle {
                width: 30; height: 30; radius: 15
                color: dialog.kind === "error" ? Theme.red : Theme.gold
                Text {
                    anchors.centerIn: parent
                    text: dialog.kind === "error" ? "!" : (dialog.question ? "?" : "i")
                    color: Theme.ink; font.family: Theme.serif; font.bold: true; font.pixelSize: 17
                }
            }
            Text {
                Layout.fillWidth: true
                text: dialog.title || "Uncharted Launcher"
                color: Theme.text; font.family: Theme.sansSemi; font.pixelSize: Theme.fontSection
                elide: Text.ElideRight
            }
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(360, messageText.implicitHeight)
            clip: true
            Text {
                id: messageText
                width: dialog.availableWidth
                text: dialog.message + (dialog.detail ? "\n\n" + dialog.detail : "")
                color: Theme.muted; font.pixelSize: Theme.fontBody
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            spacing: 10
            PillButton {
                visible: dialog.question
                text: dialog.kind === "yesno" || dialog.kind === "yesnocancel" ? "No"
                      : (dialog.kind === "retrycancel" ? "Cancel" : "Cancel")
                onClicked: dialog.reply(false)
            }
            PillButton {
                visible: dialog.kind === "yesnocancel"
                text: "Cancel"
                onClicked: dialog.reply(null)
            }
            PillButton {
                kind: "primary"
                text: dialog.question ? (dialog.kind === "okcancel" ? "OK"
                                         : (dialog.kind === "retrycancel" ? "Retry" : "Yes")) : "OK"
                onClicked: dialog.reply(true)
            }
        }
    }
}
