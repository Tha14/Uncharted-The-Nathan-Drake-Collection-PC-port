import QtQuick
import QtQuick.Layouts

// One element of a page as qt_backend.py describes it: a section, a row, a group of elements or a control.
Item {
    id: root
    property var node: ({})
    readonly property string kind: node ? (node.t || "") : ""
    // the width it needs on one line: a text's unwrapped width, a group's items side by side, else implicitWidth
    readonly property real naturalWidth: kind === "label" && !(loader.item && loader.item.naturalWidth !== undefined)
        ? labelMetrics.advanceWidth + 6
        : (loader.item && loader.item.naturalWidth !== undefined ? loader.item.naturalWidth
                                                                  : (loader.item ? loader.item.implicitWidth : 0))
    implicitWidth: naturalWidth
    TextMetrics {
        id: labelMetrics
        text: root.kind === "label" ? (root.node.text || "") : ""
        font.pixelSize: root.px(root.node.size)
        font.family: root.node.serif ? Theme.serif : (root.node.bold ? Theme.sansSemi : Theme.sans)
        font.bold: !!root.node.serif && !!root.node.bold
    }
    implicitHeight: loader.item ? loader.item.implicitHeight : 0
    height: implicitHeight

    function px(points) { return Math.round((points || 9) * 1.333) }

    Loader {
        id: loader
        width: root.width
        sourceComponent: {
            switch (root.kind) {
            case "section": return sectionC
            case "row": return rowC
            case "box": return root.node.dir === "h" ? hboxC : vboxC
            case "label": return root.node.slot || root.node.image ? slotC : labelC
            case "button": return buttonC
            case "toggle": return toggleC
            case "choice": return choiceC
            case "dropdown": return dropdownC
            case "chip": return chipC
            case "entry": return entryC
            case "log": return logC
            case "progress": return progressC
            case "card": return cardC
            case "list": return listC
            case "sep": return sepC
            case "dot": return dotC
            case "group": return groupC
            }
            return null
        }
    }

    Component { id: sectionC; SectionCard { node: root.node } }
    Component { id: rowC; SettingRow { node: root.node } }
    Component {
        id: vboxC
        Column {
            id: vcol
            spacing: 6
            // as wide as its widest item needs (a framed text box: the box's own width)
            readonly property real naturalWidth: {
                let w = 0
                for (let i = 0; i < children.length; ++i)
                    if (children[i].item) w = Math.max(w, children[i].item.naturalWidth)
                return w
            }
            Repeater {
                model: root.node.items
                Loader {
                    width: parent.width
                    source: "NodeView.qml"
                    onLoaded: { item.width = Qt.binding(() => width); item.node = Qt.binding(() => modelData) }
                    height: item ? item.implicitHeight : 0
                }
            }
        }
    }
    Component {
        id: hboxC
        // One line: buttons, chips and controls keep their size; a text takes what is left and wraps. What the
        // launcher packs on the right (a friend's Cancel) sits at the right edge; the rest wraps when it does not fit.
        Item {
            id: hbox
            readonly property var lefts: root.node.items.filter(n => n.side !== "right")
            readonly property var rights: root.node.items.filter(n => n.side === "right")
            readonly property real spacing: 10
            readonly property real naturalWidth: hrow.naturalWidth + (rights.length ? rightRow.width + spacing : 0)
            implicitHeight: Math.max(hrow.height, rightRow.height)

            Component {
                id: cellC
                Loader {
                    id: cell
                    property real room: 0
                    property int count: 1
                    source: "NodeView.qml"
                    readonly property bool grows: modelData.t === "label"
                    readonly property bool fills: modelData.t === "box" || modelData.t === "log" || modelData.t === "progress"
                    readonly property bool shares: modelData.t === "card"   // cards side by side share the line
                    onLoaded: item.node = Qt.binding(() => modelData)
                    // the cell's width (the Loader sizes its item to it)
                    width: !item ? 0 : shares ? (room - hbox.spacing * (count - 1)) / count
                        : fills ? room
                        : grows ? Math.max(40, Math.min(item.naturalWidth, room))
                        : Math.min(item.naturalWidth, room)
                    height: item ? item.implicitHeight : 0
                }
            }
            Flow {
                id: hrow
                width: Math.max(40, root.width - (hbox.rights.length ? rightRow.width + hbox.spacing : 0))
                spacing: hbox.spacing
                readonly property real naturalWidth: {
                    let w = 0, n = 0
                    for (let i = 0; i < children.length; ++i)
                        if (children[i].item) { w += children[i].item.naturalWidth; ++n }
                    return w + Math.max(0, n - 1) * spacing
                }
                Repeater {
                    model: hbox.lefts
                    delegate: cellC
                    onItemAdded: (index, item) => {
                        item.room = Qt.binding(() => hrow.width)
                        item.count = Qt.binding(() => hbox.lefts.length)
                    }
                }
            }
            Row {
                id: rightRow
                anchors.right: parent.right
                spacing: hbox.spacing
                Repeater {
                    model: hbox.rights
                    delegate: cellC
                    onItemAdded: (index, item) => { item.room = Qt.binding(() => root.width / 2) }
                }
            }
        }
    }
    Component {
        id: labelC
        Text {
            id: labelText
            readonly property var n: root.node
            // the text's one-line width (implicitWidth follows the wrapped width once a width is set)
            readonly property real naturalWidth: probe.implicitWidth + leftPadding + rightPadding + 2
            Text { id: probe; visible: false; text: labelText.text; font: labelText.font; textFormat: Text.PlainText }
            text: n.text
            width: root.width
            wrapMode: Text.Wrap
            color: n.bg && n.bg.toLowerCase() === "#e8b23e" ? Theme.ink : Theme.mapColor(n.fg)
            font.family: n.mono ? "Consolas" : (n.serif ? Theme.serif : (n.bold ? Theme.sansSemi : Theme.sans))
            font.pixelSize: root.px(n.size)
            font.bold: !!n.serif && !!n.bold
            textFormat: Text.PlainText
            padding: n.bg && n.bg.toLowerCase() === "#e8b23e" ? 3 : 0
            leftPadding: padding * 2; rightPadding: padding * 2
            Rectangle {
                visible: n.bg && n.bg.toLowerCase() === "#e8b23e"
                anchors.fill: parent; z: -1; radius: height / 2; color: Theme.gold
            }
            MouseArea {
                anchors.fill: parent
                enabled: !!n.click
                cursorShape: n.click ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: backend.act(n.id, "click")
            }
        }
    }
    // a Controls slot (a key / mouse binding or a controller button): click to rebind, right-click to clear
    Component {
        id: slotC
        Rectangle {
            readonly property var n: root.node
            readonly property bool waiting: String(n.fg).toLowerCase() === "#e8b23e"
            implicitWidth: 118
            implicitHeight: 30
            radius: 9
            color: waiting ? "#2a2416" : (slotMouse.containsMouse && n.click ? Theme.fieldHi : Theme.field)
            border.color: waiting ? Theme.gold : Theme.line
            Image {
                anchors.centerIn: parent
                visible: !!n.image
                source: backend.fileUrl(n.image || "")
                height: 22
                fillMode: Image.PreserveAspectFit
                smooth: true
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - 10
                horizontalAlignment: Text.AlignHCenter
                visible: !n.image
                text: n.text
                elide: Text.ElideRight
                color: waiting ? Theme.gold : Theme.text
                font.pixelSize: Theme.fontSmall
            }
            MouseArea {
                id: slotMouse
                anchors.fill: parent
                hoverEnabled: true
                enabled: !!n.click || !!n.rclick
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                cursorShape: n.click ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: (mouse) => backend.act(n.id, mouse.button === Qt.RightButton ? "rclick" : "click")
            }
        }
    }
    Component {
        id: buttonC
        PillButton {
            text: root.node.text
            kind: root.node.kind
            onClicked: backend.act(root.node.id, "button")
        }
    }
    Component {
        id: toggleC
        ToggleSwitch {
            checked: root.node.v
            onToggled: (value) => backend.act(root.node.id, "toggle", value)
        }
    }
    Component {
        id: choiceC
        ChoiceBar {
            options: root.node.opts
            value: root.node.v
            active: root.node.en
            onPicked: (value) => backend.act(root.node.id, "choice", value)
        }
    }
    Component {
        id: dropdownC
        DropdownBox {
            options: root.node.opts
            value: root.node.v
            label: root.node.label
            onPicked: (value) => backend.act(root.node.id, "dropdown", value)
        }
    }
    Component {
        id: chipC
        ChipButton {
            text: root.node.text
            checked: root.node.v
            onToggled: (value) => backend.act(root.node.id, "chip", value)
        }
    }
    Component {
        id: entryC
        InputField {
            text: root.node.v
            secret: root.node.secret
            onEdited: (value) => backend.act(root.node.id, "entry", value)
            onSubmitted: backend.act(root.node.id, "enter")
            buttonText: root.node.button ? root.node.button.text : ""
            onButtonClicked: backend.act(root.node.button.id, "button")
        }
    }
    Component { id: logC; LogView { text: root.node.text } }
    Component { id: progressC; ProgressLine { value: root.node.v } }
    Component {
        id: cardC
        PresetCard {
            node: root.node
        }
    }
    Component {
        id: listC
        Rectangle {
            implicitHeight: Math.min(220, listCol.implicitHeight + 12)
            radius: Theme.radiusField
            color: Theme.field
            border.color: Theme.line
            clip: true
            Column {
                id: listCol
                x: 6; y: 6
                width: parent.width - 12
                Repeater {
                    model: root.node.items
                    Rectangle {
                        width: listCol.width
                        height: 28
                        radius: 8
                        readonly property bool sel: (root.node.sel || []).indexOf(index) >= 0
                        color: sel ? Theme.gold : (itemMouse.containsMouse ? Theme.fieldHi : "transparent")
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10
                            text: modelData
                            color: parent.sel ? Theme.ink : Theme.text
                            font.pixelSize: Theme.fontSmall
                        }
                        MouseArea {
                            id: itemMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: backend.act(root.node.id, "list", [index])
                        }
                    }
                }
            }
        }
    }
    Component {
        id: dotC
        Item {
            implicitWidth: 14; implicitHeight: 14
            Rectangle { anchors.centerIn: parent; width: 10; height: 10; radius: 5; color: root.node.color || Theme.dim }
        }
    }
    Component { id: groupC; CollapsibleGroup { node: root.node } }
    Component { id: sepC; Item { implicitHeight: 9; Rectangle { y: 4; width: parent.width; height: 1; color: Theme.cardLine } } }
}
