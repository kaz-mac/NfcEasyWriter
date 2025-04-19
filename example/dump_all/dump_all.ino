/*
  NfcEasyWriter Example
  HEXダンプ

  想定するNFCカード: MIFARE Classic, NTAG213/215/216
  想定するRFIDリーダー: M5Stack RFID 2 Unit (WS1850S)
  別途必要なライブラリ: MFRC522_I2C
*/
#include <M5Unified.h>

#include "NfcEasyWriter.h"
MFRC522_I2C_Extend mfrc522(0x28, -1, &Wire); // I2C address, dummy, Wire
NfcEasyWriter nfc(mfrc522);

// デバッグに便利なマクロ定義 --------
#define sp(x) Serial.println(x)
#define spn(x) Serial.print(x)
#define spf(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#define spp(k,v) Serial.println(String(k)+"="+String(v))


void setup() {
  // M5Stack 初期設定
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  int8_t pinSda = M5.getPin(m5::pin_name_t::port_a_sda);
  int8_t pinScl = M5.getPin(m5::pin_name_t::port_a_scl);
  Wire.begin(pinSda, pinScl);

  // RFIDリーダーの初期化
  nfc.init();
  nfc._debug = false;  // for debug
}

void loop() {
  // マウント
  spn("\nカードを置いてください..");
  while (!nfc.mountCard(1000)) {
    spn(".");
  }
  sp("認識しました");
  delay(1000);

  // ダンプする
  nfc.dumpAll();

  // アンマウント
  nfc.unmountCard();

  // 終了
  sp("\n\n\nボタンを押すと繰り返します");
  while (1) {
    M5.update();
    if (M5.BtnA.wasPressed()) break;
    delay(10);
  }
}
