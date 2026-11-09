import QtQuick
import QtQuick.Effects

// Frosted glass: what lies behind the panel (backdrop) blurred, inside rounded corners, under a dark tint.
Item {
    id: glass
    property Item backdrop: null
    property real radius: 16
    property color tint: "#8c101014"
    property real blurAmount: 1.0

    ShaderEffectSource {
        id: behind
        anchors.fill: parent
        visible: false
        sourceItem: glass.backdrop
        live: true
        recursive: false
        sourceRect: {
            if (!glass.backdrop) return Qt.rect(0, 0, 0, 0)
            const p = glass.mapToItem(glass.backdrop, 0, 0)
            return Qt.rect(p.x, p.y, glass.width, glass.height)
        }
    }
    Rectangle {
        id: mask
        anchors.fill: parent
        radius: glass.radius
        visible: false
        layer.enabled: true
    }
    MultiEffect {
        anchors.fill: parent
        visible: !!glass.backdrop
        source: behind
        blurEnabled: true
        blurMax: 48
        blur: glass.blurAmount
        maskEnabled: true
        maskSource: mask
    }
    Rectangle {
        anchors.fill: parent
        radius: glass.radius
        color: glass.tint
    }
}
