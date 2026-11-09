import QtQuick

// A rounded text box; each change goes to the launcher (as typing in the Tk entry).
Rectangle {
    id: field
    property string text: ""
    property bool secret: false
    signal edited(string value)
    signal submitted()
    property string buttonText: ""          // a button inside the box, at the right (Add friend's ADD)
    signal buttonClicked()
    implicitWidth: 240
    implicitHeight: 36
    radius: Theme.radiusField
    color: Theme.field
    border.color: input.activeFocus ? Theme.gold : Theme.line

    TextInput {
        id: input
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: field.buttonText ? inner.width + 18 : 12
        verticalAlignment: TextInput.AlignVCenter
        color: Theme.text
        selectionColor: Theme.gold
        selectedTextColor: Theme.ink
        font.pixelSize: Theme.fontBody
        echoMode: field.secret ? TextInput.Password : TextInput.Normal
        clip: true
        onTextEdited: field.edited(text)
        onAccepted: field.submitted()
        // the launcher's value, unless the user is typing in it
        Binding on text { value: field.text; when: !input.activeFocus }
    }
    PillButton {
        id: inner
        visible: !!field.buttonText
        kind: "primary"
        text: field.buttonText
        anchors.right: parent.right
        anchors.rightMargin: 5
        anchors.verticalCenter: parent.verticalCenter
        height: parent.height - 10
        onClicked: field.buttonClicked()
    }
}
