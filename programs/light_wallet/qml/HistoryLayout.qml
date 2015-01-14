import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Layouts 1.1

import Material 0.1

Item {
   property real minimumWidth: 30
   property real minimumHeight: units.dp(80)
   property string accountName
   property string assetSymbol

   ScrollView {
      id: historyList
      anchors.fill: parent
      flickableItem.interactive: true

      ListView {
         model: wallet.account.transactionHistory(assetSymbol)
         delegate: Rectangle {
            width: parent.width - visuals.margins * 2
            height: transactionSummary.height
            anchors.horizontalCenter: parent.horizontalCenter
            property var trx: model

            Rectangle { width: parent.width; height: 1; color: "darkgrey"; visible: index }
            ColumnLayout {
               id: transactionSummary
               width: parent.width

               Item { Layout.preferredHeight: visuals.margins }
               RowLayout {
                  width: parent.width

                  Item { Layout.preferredWidth: visuals.margins }
                  Label {
                     text: qsTr("Transaction #") + trx.modelData.id.substring(0, 8)
                     font.pixelSize: visuals.textBaseSize
                     Layout.fillWidth: true
                  }
                  Label {
                     text: model.modelData.timestamp
                     font.pixelSize: visuals.textBaseSize
                  }
                  Item { Layout.preferredWidth: visuals.margins }
               }
               Item { Layout.preferredHeight: visuals.margins }
               Repeater {
                  id: ledgerRepeater
                  model: trx.modelData.ledger
                  delegate: RowLayout {
                     width: parent.width
                     property real senderWidth: senderLabel.width

                     Item { Layout.preferredWidth: visuals.margins }
                     Label {
                        id: senderLabel
                        text: sender
                        Layout.preferredWidth: (text === "" && index)?
                                                  ledgerRepeater.itemAt(index - 1).senderWidth : implicitWidth
                        font.pixelSize: visuals.textBaseSize * .75
                        elide: Text.ElideRight
                        Layout.maximumWidth: historyList.width / 4
                     }
                     Label {
                        text: "→ " + amount + " " + symbol + " →"
                        font.pointSize: visuals.textBaseSize * .75
                        Layout.minimumWidth: implicitWidth
                     }
                     Label {
                        text: receiver
                        font.pixelSize: visuals.textBaseSize * .75
                        Layout.maximumWidth: historyList.width / 3
                     }
                     Label {
                        text: memo
                        font.pixelSize: visuals.textBaseSize * .75
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                     }
                     Item { Layout.preferredWidth: visuals.margins }
                  }
               }
               Item { Layout.preferredHeight: visuals.margins }
            }
         }
      }
   }
}