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

    // one section alone (Quick Setup, Cheats) takes the whole width
    readonly property bool single: items.filter(n => n.t === "section").length === 1
    readonly property var wide: items.filter(n => n.t !== "section" || single)
    // a very long section (RPCN with its friend list) gets the right column to itself; the others stack on the left
    readonly property bool pinned: items.some(n => n.t === "section" && n.column === "right")
    readonly property var full: single ? [] : items.filter(n => n.t === "section"
                                                             && (pinned ? n.column === "right" : weight(n) >= 40))
    readonly property var sections: single ? [] : items.filter(n => n.t === "section"
                                                                 && (pinned ? n.column !== "right" : weight(n) < 40))

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
    // being cut straight: the page is drawn through this mask.
    readonly property real edgeRadius: Theme.radiusCard
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
        Repeater {   // a page's intro (the multiplayer banner, a note): a full-width card of its own
            model: view.wide
            SectionCard {
                width: content.width
                node: modelData.t === "section" ? modelData : ({ t: "section", title: "", note: "", items: [modelData] })
            }
        }
        Row {
            spacing: view.gap
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
    }
}
