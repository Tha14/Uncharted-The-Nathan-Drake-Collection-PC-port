import QtQuick

// A controller button's icon, drawn here (Xbox or PlayStation style): pad = {"buttons": [names], "labels": [texts],
// "style"}; several buttons (a combination) side by side with a plus between them.
Row {
    id: icon
    property var pad: ({})
    property real size: 22
    readonly property bool ps: pad.style === "playstation"
    spacing: 0

    Repeater {
        model: icon.pad.buttons || []
        Row {
            spacing: 0
            Canvas {
                id: glyph
                readonly property string button: modelData
                readonly property string label: (icon.pad.labels || [])[index] || modelData
                readonly property real widthFactor: {
                    if (["a", "b", "x", "y", "leftstick", "rightstick", "lstick", "rstick", "start", "back"].indexOf(button) >= 0
                            || button.indexOf("dp") === 0)
                        return 1.0
                    if (button === "leftshoulder" || button === "rightshoulder") return 1.55
                    if (button === "lefttrigger" || button === "righttrigger") return 1.25
                    return Math.max(1.0, 0.36 * label.length)
                }
                width: Math.round(icon.size * widthFactor)
                height: icon.size
                antialiasing: true
                onPaint: {
                    const c = getContext("2d")
                    const W = width, H = height, line = Math.max(1, H / 12)
                    const body = "#202022", rim = "#969696", ink = "#f0f0f0"
                    c.reset()
                    c.lineWidth = line
                    function roundRect(x0, y0, x1, y1, r) {
                        c.beginPath()
                        c.moveTo(x0 + r, y0); c.lineTo(x1 - r, y0); c.arcTo(x1, y0, x1, y0 + r, r)
                        c.lineTo(x1, y1 - r); c.arcTo(x1, y1, x1 - r, y1, r); c.lineTo(x0 + r, y1)
                        c.arcTo(x0, y1, x0, y1 - r, r); c.lineTo(x0, y0 + r); c.arcTo(x0, y0, x0 + r, y0, r)
                        c.closePath()
                    }
                    function ellipse(x0, y0, x1, y1) {
                        c.beginPath(); c.ellipse(x0, y0, x1 - x0, y1 - y0); c.closePath()
                    }
                    function filled(color) { c.fillStyle = color; c.fill() }
                    function outlined(color) { c.strokeStyle = color; c.stroke() }
                    function text(t, color, factor, dy) {
                        c.fillStyle = color
                        c.font = "bold " + Math.max(4, Math.round(H * factor)) + "px 'Segoe UI'"
                        c.textAlign = "center"; c.textBaseline = "middle"
                        c.fillText(t, W / 2, H / 2 + (dy || 0))
                    }
                    const b = button
                    if (["a", "b", "x", "y"].indexOf(b) >= 0) {
                        ellipse(line, line, W - line, H - line); filled(body); outlined(rim)
                        if (icon.ps) {
                            const color = {"a": "#7c9cff", "b": "#ff6666", "x": "#f08cd2", "y": "#40d6a0"}[b]
                            const m = W / 2, r = H * 0.22
                            c.lineWidth = Math.max(1, H * 0.08); c.strokeStyle = color
                            c.beginPath()
                            if (b === "a") { c.moveTo(m - r, m - r); c.lineTo(m + r, m + r); c.moveTo(m - r, m + r); c.lineTo(m + r, m - r) }
                            else if (b === "b") c.ellipse(m - r, m - r, 2 * r, 2 * r)
                            else if (b === "x") c.rect(m - r * 0.85, m - r * 0.85, r * 1.7, r * 1.7)
                            else { c.moveTo(m, m - r * 1.05); c.lineTo(m - r * 1.1, m + r * 0.75); c.lineTo(m + r * 1.1, m + r * 0.75); c.closePath() }
                            c.stroke()
                        } else {
                            text(b.toUpperCase(), {"a": "#6cc24a", "b": "#e5473b", "x": "#3f8ee6", "y": "#f4c430"}[b], 0.6)
                        }
                    } else if (b === "leftshoulder" || b === "rightshoulder") {
                        roundRect(line, H * 0.14, W - line, H * 0.86, H * 0.3); filled(body); outlined(rim)
                        text(label, ink, 0.46)
                    } else if (b === "lefttrigger" || b === "righttrigger") {
                        roundRect(line, line, W - line, H - line, H * 0.32); filled(body); outlined(rim)
                        c.fillStyle = body; c.fillRect(line * 2, H * 0.55, W - line * 4, H * 0.45 - line * 2)
                        text(label, ink, 0.44)
                    } else if (["leftstick", "rightstick", "lstick", "rstick"].indexOf(b) >= 0) {
                        ellipse(line, line, W - line, H - line); filled(body); outlined(rim)
                        if (b === "lstick" || b === "rstick") {
                            ellipse(H * 0.24, H * 0.24, H * 0.76, H * 0.76); outlined(rim)
                            text(b === "lstick" ? "L" : "R", ink, 0.38)
                        } else {
                            text(label, ink, 0.4)
                        }
                    } else if (b.indexOf("dp") === 0) {
                        const a1 = H * 0.31, b1 = H * 0.69
                        c.beginPath(); c.rect(a1, line, b1 - a1, H - 2 * line); filled(body); outlined(rim)
                        c.beginPath(); c.rect(line, a1, H - 2 * line, b1 - a1); filled(body); outlined(rim)
                        c.fillStyle = body
                        c.fillRect(a1 + line, a1, b1 - a1 - 2 * line, b1 - a1)
                        c.fillRect(a1, a1 + line, b1 - a1, b1 - a1 - 2 * line)
                        const arms = {"dpup": [a1, line, b1, a1 + line], "dpdown": [a1, b1 - line, b1, H - line],
                                      "dpleft": [line, a1, a1 + line, b1], "dpright": [b1 - line, a1, H - line, b1]}
                        const arm = arms[b]
                        if (arm) { c.fillStyle = "#fac85a"; c.fillRect(arm[0] + line, arm[1] + line, arm[2] - arm[0] - 2 * line, arm[3] - arm[1] - 2 * line) }
                    } else if (b === "start" || b === "back") {
                        ellipse(H * 0.1, H * 0.1, H * 0.9, H * 0.9); filled(body); outlined(rim)
                        const m = H / 2
                        c.strokeStyle = ink; c.lineWidth = line
                        if (b === "start") {
                            c.beginPath()
                            for (const k of [-1, 0, 1]) { const y = m + k * H * 0.13; c.moveTo(m - H * 0.17, y); c.lineTo(m + H * 0.17, y) }
                            c.stroke()
                        } else if (icon.ps) {
                            c.beginPath(); c.moveTo(m, m - H * 0.18); c.lineTo(m, m + H * 0.16)
                            c.moveTo(m - H * 0.12, m - H * 0.06); c.lineTo(m, m - H * 0.18); c.lineTo(m + H * 0.12, m - H * 0.06); c.stroke()
                        } else {
                            c.strokeRect(m - H * 0.17, m - H * 0.15, H * 0.22, H * 0.2)
                            c.fillStyle = ink; c.fillRect(m - H * 0.05, m - H * 0.03, H * 0.22, H * 0.2)
                        }
                    } else {
                        roundRect(line, H * 0.14, W - line, H * 0.86, H * 0.25); filled(body); outlined(rim)
                        text(label, ink, 0.4)
                    }
                }
                Component.onCompleted: requestPaint()
                onButtonChanged: requestPaint()
            }
            // a plus between the buttons of a combination
            Canvas {
                visible: index + 1 < (icon.pad.buttons || []).length
                width: visible ? Math.round(icon.size / 2) : 0
                height: icon.size
                onPaint: {
                    const c = getContext("2d"), r = Math.max(2, icon.size / 6), m = width / 2, y = height / 2
                    c.reset(); c.strokeStyle = "#f0f0f0"; c.lineWidth = Math.max(1, icon.size / 12)
                    c.beginPath(); c.moveTo(m - r, y); c.lineTo(m + r, y); c.moveTo(m, y - r); c.lineTo(m, y + r); c.stroke()
                }
                Component.onCompleted: requestPaint()
            }
        }
    }
}
