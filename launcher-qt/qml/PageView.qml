import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// The shown tab's page. What is not in a section (an intro, the multiplayer banner) spans the width; the sections
// (rounded cards) sit side by side in two columns, each card going to the shorter column. One column when the
// window is narrow. Scrolls when taller than the window.
Flickable {
    id: view
    property var items: []
    property string tab: ""
    property var positions: ({})   // each tab keeps its scroll position
    readonly property int columns: width >= 900 ? 2 : 1
    readonly property real gap: 16
    readonly property real innerWidth: width - 2 * Theme.gutter - 6
    readonly property real columnWidth: (innerWidth - (columns - 1) * gap) / columns

    // a section marked "wide" (the Controls bindings) spans the width under the columns
    readonly property var below: items.filter(n => n.t === "section" && n.column === "wide")
    readonly property var columned: items.filter(n => n.t === "section" && n.column !== "wide")
    // one section alone (Setup) takes the whole width
    readonly property bool single: columned.length === 1 && below.length === 0
    readonly property var wide: items.filter(n => n.t !== "section" || single)
    // a section can ask for the right column (RPCN with its friend list, Cheats' Unlocks); the others stack on the left
    readonly property bool pinned: columned.some(n => n.column === "right")
    readonly property var full: single ? [] : columned.filter(n => pinned ? n.column === "right" : weight(n) >= 40)
    readonly property var sections: single ? [] : columned.filter(n => pinned ? n.column !== "right" : weight(n) < 40)

    // a section's rough height, to put each in the shorter column (rows count most, long notes and logs too)
    function weight(n) {
        if (!n) return 0
        if (n.t === "row") return 3 + (n.hint ? n.hint.length / 110 : 0)
        if (n.t === "log") return 9
        if (n.t === "label") return 1 + n.text.length / 120
        if (n.t === "sep") return 0.3
        if (n.items) return n.items.reduce((a, c) => a + weight(c), n.t === "section" ? 2.5 : 0)
        return 2
    }
    readonly property var split: {
        if (full.length && columns === 2)
            return [sections, full]
        if (full.length)
            return [sections.concat(full)]
        const cols = [], load = []
        for (let i = 0; i < columns; ++i) { cols.push([]); load.push(0) }
        for (const s of sections) {
            let k = 0
            for (let i = 1; i < columns; ++i) if (load[i] < load[k]) k = i
            cols[k].push(s)
            load[k] += weight(s)
        }
        return cols
    }

    clip: true
    // Where the page meets the tabs and the PLAY bar, the cards curve away (rounded corners per column) instead of
    // being cut straight: the page is drawn through this mask. Where a full-width card touches the top or the bottom
    // edge, that edge is rounded across the whole width instead, so the card has no notch at the column gap.
    readonly property real edgeRadius: Theme.radiusCard
    readonly property real viewBottom: contentY + height
    readonly property bool topWide: (wideBlock.visible && contentY < wideBlock.y + wideBlock.height)
                                    || (belowBlock.visible && contentY >= belowBlock.y - gap)
    readonly property bool bottomWide: (wideBlock.visible && viewBottom <= wideBlock.y + wideBlock.height + gap)
                                       || (belowBlock.visible && viewBottom > belowBlock.y)
    property Item viewMask: Item {
        parent: view   // the Flickable itself, not its scrolling content
        width: view.width
        height: view.height
        visible: false
        layer.enabled: true
        Rectangle {   // the middle (the gap between the columns too, so a full-width card has no seam)
            x: Theme.gutter
            y: view.edgeRadius
            width: view.innerWidth + 6
            height: Math.max(0, view.height - 2 * view.edgeRadius)
        }
        Repeater {   // each column, with rounded ends
            model: view.columns
            Rectangle {
                x: Theme.gutter + index * (view.columnWidth + view.gap)
                width: view.columns === 1 ? view.innerWidth : view.columnWidth
                height: view.height
                radius: view.edgeRadius
            }
        }
        // a full-width card at the top or bottom edge: one rounded strip across the width there (switched by its
        // height: everything in this hidden mask item counts as invisible, so `visible` cannot switch it)
        Rectangle {
            x: Theme.gutter
            width: view.innerWidth
            height: view.topWide ? 2 * view.edgeRadius : 0
            radius: view.edgeRadius
        }
        Rectangle {
            x: Theme.gutter
            y: view.height - 2 * view.edgeRadius
            width: view.innerWidth
            height: view.bottomWide ? 2 * view.edgeRadius : 0
            radius: view.edgeRadius
        }
        Rectangle { x: view.width - 14; width: 14; height: view.height }   // the scroll bar
    }
    layer.enabled: true
    layer.effect: MultiEffect {
        maskEnabled: true
        maskSource: view.viewMask
    }
    contentWidth: width
    contentHeight: content.implicitHeight + 24
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {
        policy: view.contentHeight > view.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
        width: 10
        contentItem: Rectangle { implicitWidth: 6; radius: 3; color: parent.pressed ? Theme.muted : Theme.line }
        background: Item {}
    }

    // a page that changes keeps its scroll position; another tab opens where it was left (at the top the first time)
    property real keep: 0
    property string shownTab: ""
    onItemsChanged: {
        if (tab !== shownTab) {
            shownTab = tab
            keep = positions[tab] || 0
        } else {
            keep = contentY
        }
        Qt.callLater(() => { view.contentY = Math.max(0, Math.min(keep, contentHeight - height)) })
    }
    onContentYChanged: if (shownTab === tab && settled) positions[tab] = contentY
    // a tab opens at its own place (the top the first time), once its page has been laid out
    property bool settled: false
    onTabChanged: {
        settled = false
        settle.restart()
    }
    Timer {
        id: settle
        interval: 150
        onTriggered: { view.contentY = view.positions[view.tab] || 0; view.settled = true }
    }
    Component.onCompleted: settle.start()

    Column {
        id: content
        x: Theme.gutter
        width: view.innerWidth
        spacing: view.gap
        Column {   // a page's intro (the multiplayer banner, a note): a full-width card of its own
            id: wideBlock
            width: content.width
            spacing: view.gap
            visible: view.wide.length > 0
            Repeater {
                model: view.wide
                SectionCard {
                    width: content.width
                    node: modelData.t === "section" ? modelData : ({ t: "section", title: "", note: "", items: [modelData] })
                }
            }
        }
        Row {
            spacing: view.gap
            visible: view.split.some(c => c.length > 0)
            Repeater {
                model: view.split
                Column {
                    width: view.columnWidth
                    spacing: view.gap
                    Repeater {
                        model: modelData
                        NodeView { width: view.columnWidth; node: modelData }
                    }
                }
            }
        }
        Column {   // full-width sections under the columns (the Controls bindings)
            id: belowBlock
            width: content.width
            spacing: view.gap
            visible: view.below.length > 0
            Repeater {
                model: view.below
                NodeView { width: content.width; node: modelData }
            }
        }
    }
}
