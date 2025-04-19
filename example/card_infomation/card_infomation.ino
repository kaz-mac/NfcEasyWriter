/*
  NfcEasyWriter Example
  NFCカードの認識、NFCカードの情報表示

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

  // カード情報を表示する
  sp("\n**** カード情報 ****");
  spf("UID: %s\n", nfc.getUidString().c_str());
  if (nfc.isClassic()) {
    sp("カード種別: MIFARE Classic");
    spf("使用可能セクタ範囲: %d～%d\n", nfc._minSectorCL, nfc._maxSectorCL);
  } else if (nfc.isUltralight()) {
    // NTAG種別
    spn("カード種別: ");
    if (nfc._ntagType == NT_NTAG213) sp("NTAG213");
    else if (nfc._ntagType == NT_NTAG215) sp("NTAG215");
    else if (nfc._ntagType == NT_NTAG216) sp("NTAG216");
    else sp("unknown");
    spf("使用可能ページ範囲: %d～%d\n", nfc._minPageUL, nfc._maxPageUL);
    spf("設定ページ: %d\n", nfc._configPageUL);
  } else {
    sp("無効なカード");
  }

  // 使用可能な容量（仮想アドレス換算）を取得する
  uint16_t totalSize = nfc.getVCapacities();
  spf("使用可能な容量: %d バイト\n", totalSize);

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
