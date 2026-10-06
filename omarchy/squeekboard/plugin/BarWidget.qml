import QtQuick
import qs.Commons
import qs.Ui

BarWidget {
    id: root
    moduleName: "andromarchy.squeekboard"

    implicitWidth: button.implicitWidth
    implicitHeight: button.implicitHeight

    WidgetButton {
        id: button
        anchors.fill: parent
        bar: root.bar
        text: "⌨"
        tooltipText: "On-Screen Keyboard"
        onPressed: function(buttonCode) {
            if (root.bar)
                root.bar.run("squeekboard-toggle")
        }
    }
}
