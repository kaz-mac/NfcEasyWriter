/*
  rfid_class_test2.ino
  NfcEasyWriterクラスの動作テストをする

  想定するNFCカード: MIFARE Classic, NTAG213/215/216
  想定するRFIDリーダー: M5Stack RFID 2 Unit (WS1850S)
  別途必要なライブラリ: MFRC522_I2C

  Copyright (c) 2025 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT
*/
//#include <M5Stack.h>
#include <M5Unified.h>

// NFC関連
// #include <MFRC522_I2C.h>   // オリジナルのクラス
#include "NfcEasyWriter.h"
MFRC522_I2C_Extend mfrc522(0x28, -1, &Wire); // デバイスアドレス、dummy、TwoWireインスタンスを指定してインスタンス化（&Wire省略可）
NfcEasyWriter nfc(mfrc522);

// デバッグに便利なマクロ定義 --------
#define sp(x) Serial.println(x)
#define spn(x) Serial.print(x)
#define spf(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#define spp(k,v) Serial.println(String(k)+"="+String(v))
#define array_length(x) (sizeof(x) / sizeof(x[0]))

// enum CardType : uint8_t { UnknownCard, Classic, Ultralight };
struct Points {
  int point = 0;
  int total = 0;
};

//
// ユーティリティ ------------------------------------------------------
//

// PASS/FAIL
String tf(bool res) {
  return res ? "PASS" : "FAIL";
}

// [Classic] 壊してしまったセクタートレーラーを修復する
void recoverCardCL() {
  uint8_t blkSta = 7; // 4の倍数+3であること
  uint8_t blkEnd = 7;
  bool useKeyB = true;
  AuthKey passwd = {{ 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6 }};
  // AuthKey passwd = {{ 0,0,0,0,0,0 }};
  if (blkSta % 4 != 3 || blkEnd % 4 != 3) return;

  if (nfc.waitCard(5000)) {
    sp("\n\n\n**** 壊してしまったセクタートレーラーを修復します ****");
    for (uint8_t blk=blkSta; blk<=blkEnd; blk+=4) {
      bool res = nfc.recoverySectorTruckCL(blk, &passwd, useKeyB);
      spf("修復 Block=%d : %s\n", blk, tf(res));
    }
  } else {
    sp("error");
  }
}

// [Ultralight] 壊してしまった設定情報を修復する
void recoverCardUL() {
  ProtectMode mode = PRT_PASSWD_RW;
  AuthKey passwd = {{ 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6 }};
  //AuthKey passwd = {{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }};

  sp("\n\n\n**** 壊してしまった設定情報を修復します ****");
  if (nfc.waitCard(5000)) {
    bool res = nfc.recoveryConfigDataUL(true, &passwd, mode);
    spf("修復 page=%d : %s\n", nfc._configPageUL, tf(res));
  } else {
    sp("error");
  }
}

// [Ultralight] 設定情報の読み込み詳細を表示する
bool showConfigInfo(ULConfigEx* ulconfex, bool exinfo=false, ProtectMode mode=PRT_AUTO) {
  if (nfc.readConfigDataUL(ulconfex, mode)) {
    spn("MIRROR:  ");  spf("%02X : ", ulconfex->ulconf.MIRROR);  nfc.printDumpBin(&ulconfex->ulconf.MIRROR, 1);
    spn("MIRRORP: ");  spf("%02X : ", ulconfex->ulconf.MIRROR_PAGGE);  nfc.printDumpBin(&ulconfex->ulconf.MIRROR_PAGGE, 1);
    spn("AUTH0:   ");  spf("%02X : ", ulconfex->ulconf.AUTH0);  nfc.printDumpBin(&ulconfex->ulconf.AUTH0, 1);
    spn("ACCESS:  ");  spf("%02X : ", ulconfex->ulconf.ACCESS);  nfc.printDumpBin(&ulconfex->ulconf.ACCESS, 1);
    spn("PWD4:    ");  nfc.printDump1Line(ulconfex->ulconf.PWD4, 4);
    spn("PACK:    ");  nfc.printDump1Line(ulconfex->ulconf.PACK, 2);
    if (exinfo) {
      sp("----詳細情報----");
      spf("パスワード認証が必要なページ: %d以降\n", ulconfex->ulconf.AUTH0);
      spf("プロテクト状態: %s\n", (ulconfex->PROT ? "読み書きは要PW認証(1b)" : "書き込み時のみPW認証(0b)"));
      spf("設定のロック状態: %s\n", (ulconfex->CFGLCK ? "永久ロック状態" : "書き換え可能"));
      spf("認証失敗の許容上限: %d回まで\n", ulconfex->AUTHLIM);
      sp("----");
    }
    return true;
  } else {
    sp("showConfigInfo() read error");
    return false;
  }
}

// ポイント加算
void judge(bool flag, Points* pt) {
  if (pt == nullptr) return;
  if (flag) pt->point++;
  pt->total++;
}

// 書き込み・読み込み・比較のサブルーチン
#define TEST_WRITE 1
#define TEST_READ 2
#define TEST_READ_COMPARE 3
AuthKey passwdDefault = {{  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }};
AuthKey passwdGood = {{ 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6 }};
AuthKey passwdBad = {{ 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6 }};
bool subTest(uint16_t vindex, byte offset, size_t tnum, bool invert, uint8_t command, String title, Points* pt, ProtectMode mode=PRT_AUTO) {
  byte wdata[tnum], rdata[tnum];
  bool res = false, res2;
  for (int i=0; i<sizeof(wdata); i++) wdata[i] = i + offset;

  sp("----\n【小テスト "+title+"】");
  if (command == TEST_WRITE) {
    res2 = nfc.writeData(vindex, wdata, sizeof(wdata), mode);  // 書き込み
    res = (res2 ^ invert);
    spp((invert ? "書き込めないこと" : "書き込み"), tf(res));
  } else if (command == TEST_READ || command == TEST_READ_COMPARE) {
    memset(rdata, 0, sizeof(rdata));
    res2 = nfc.readData(vindex, rdata, sizeof(rdata), mode);   // 読み込み
    if (command == TEST_READ) {
      res = (res2 ^ invert);
      spp((invert ? "読み込めないこと" : "読み込み"), tf(res));
    } else if (command == TEST_READ_COMPARE) {
      spp((invert ? "読み込めないこと" : "読み込み"), tf(res2 ^ invert));
      res2 = (memcmp(wdata, rdata, sizeof(wdata)) == 0);   // 比較
      res = (res2 ^ invert);
      spp((invert ? "一致しないこと" : "一致する"), tf(res));
    }
  }

  judge(res, pt);
  spp("◎テスト結果 "+title, ((res) ? "PASS" : "***** FAIL *****"));
  return res;
}

//
// テストルーチン（基本） ------------------------------------------------------
//

// [共通] 基本テスト　配列のデータを読み書き
bool testArrayData() {
  bool res;
  uint16_t point = 0;
  uint16_t vindex = 0;

  sp("\n\n\n**** 配列のデータを読み書きします ****");
  byte wdata[32], rdata[32];
  for (int i=0; i<sizeof(wdata); i++) wdata[i] = i+1;
  res = nfc.writeData(vindex, wdata, sizeof(wdata));  // ←第3引数は省略できる
  if (res) point++;
  // spn("  wdata: "); nfc.printDump1Line(wdata, sizeof(wdata));
  spp("書き込み", tf(res));
  res = nfc.readData(vindex, rdata, sizeof(rdata));  // ←第3引数は省略できる
  if (res) point++;
  // spn("  rdata: "); nfc.printDump1Line(rdata, sizeof(rdata));
  spp("読み込み", tf(res));
  res = (memcmp(wdata, rdata, sizeof(wdata)) == 0);
  if (res) point++;

  // 最終判定
  res = (point == 3);
  spf("================\n■最終判定 %s (%d/3)\n================\n", tf(res), point);
  return res;
}

// [共通] 基本テスト　構造体のデータを読み書き
bool testStructData() {
  bool res;
  uint16_t point = 0;
  uint16_t vindex = 0;

  enum class DataType : uint8_t { Unknown, ItemInfo, WalletBase, WalletExinfo };
  struct NfcItem {  // 64 byte
    DataType type;
    uint16_t id;
    uint32_t price;
    char name[53];
  };

  sp("\n\n\n**** 構造体のデータを読み書きします ****");
  NfcItem itema = { DataType::ItemInfo, 1, 130, "Zunda Juice ずんだジュース" };
  res = nfc.writeData(vindex, &itema, sizeof(NfcItem));
  if (res) point++;
  spp("書き込み", tf(res));
  NfcItem itemb;
  res = nfc.readData(vindex, &itemb, sizeof(NfcItem));
  if (res) point++;
  spp("読み込み", tf(res));
  spp("  data: id",itemb.id);
  spp("  data: price",itemb.price);
  spp("  data: name",itemb.name);
  res = (itema.id == itemb.id && itema.price == itemb.price);
  if (res) point++;

  // 最終判定
  res = (point == 3);
  spf("================\n■最終判定 %s (%d/3)\n================\n", tf(res), point);
  return res;
}

//
// テストルーチン（応用） ------------------------------------------------------
//

// PRT_NOPASS_RW : PW認証なし・読み書きモード
bool testNopassReadwrite() {
  bool res;
  Points pt;
  uint16_t vindex = 0;
  const size_t tnum = (nfc.isClassic()) ? 32 : 16;
  ProtectMode mode;

  sp("\n\n\n**** PRT_NOPASS_RW : PW認証なし・読み書きモード : 通常の読み書きのチェックをします ****");
  mode = PRT_NOPASS_RW;

  sp("\n■通常の書き込みテスト");
  subTest(vindex, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  // subTest(vindex, 0, tnum, false, TEST_READ, "読み込み", &point, mode);
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  // 最終判定
  res = (pt.point == pt.total);
  spf("\n================\n■最終判定 %s (%d/%d)\n================\n", tf(res), pt.point, pt.total);
  return res;
}

// PRT_NOPASS_RO : PW認証なし・読み込み専用モード
bool testNopassReadonly() {
  bool res;
  Points pt;
  uint16_t vindex = 0;
  const size_t tnum = (nfc.isClassic()) ? 32 : 16;
  ProtectMode mode;

  sp("\n\n\n**** PRT_NOPASS_RO : PW認証なし・読み込み専用モード : 書き込みができないことをチェックをします ****");
  mode = PRT_NOPASS_RW;

  sp("\n■通常の書き込み(1)");
  subTest(vindex, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■モード変更  パスワード=null  PRT_NOPASS_RO");
  res = nfc.writeProtect(PRT_NOPASS_RO, nullptr, vindex, tnum, mode);
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■書き込めないはず(2)");
  subTest(vindex, 11, tnum, true, TEST_WRITE, "書き込めないこと", &pt, mode);
  subTest(vindex, 11, tnum, false, TEST_READ, "読み込みはできる", &pt, mode);
  
  sp("\n■モード変更  パスワード=null  PRT_NOPASS_RO");
  res = nfc.writeProtect(PRT_NOPASS_RW, nullptr, vindex, tnum, mode);
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■(1)で書いたデータが残っているか？");
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  // 最終判定
  res = (pt.point == pt.total);
  spf("\n================\n■最終判定 %s (%d/%d)\n================\n", tf(res), pt.point, pt.total);
  return res;
}

// PRT_PASSWD_RW : PW認証あり・読み書きモード
bool testPasswdReadwrite() {
  bool res;
  Points pt;
  uint16_t vindex = 0;
  const size_t tnum = (nfc.isClassic()) ? 32 : 16;
  ProtectMode mode;

  sp("\n\n\n**** PRT_PASSWD_RW : PW認証あり・読み書きモード : PW認証で保護されていることを確認します ****");
  mode = PRT_NOPASS_RW;

  sp("\n■通常の書き込み(1)");
  subTest(vindex, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■モード変更  パスワード=passwdGood  PRT_PASSWD_RW");
  res = nfc.writeProtect(PRT_PASSWD_RW, &passwdGood, vindex, tnum, mode);
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■使用するパスワードを passwdGood に変更する");
  nfc.setAuthKey(&passwdGood);    // 正しいパスワード

  sp("\n■正規PWで書き込み(2)");
  subTest(vindex, 11, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 11, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■KeyBで要認証のときにKeyAで読み書きができないこと（NFCはプロテクト状態のまま）");
  subTest(vindex, 33, tnum, true, TEST_WRITE, "書き込めないこと", &pt, PRT_NOPASS_RW);
  subTest(vindex, 33, tnum, true, TEST_READ, "読み込めないこと", &pt, PRT_NOPASS_RW);

  sp("\n■使用するパスワードを passwdBad に変更する");
  nfc.setAuthKey(&passwdBad);    // 不正なパスワード

  sp("\n■不正PWで書き込めないはず(3)");
  subTest(vindex, 22, tnum, true, TEST_WRITE, "書き込めないこと", &pt, mode);
  subTest(vindex, 22, tnum, true, TEST_READ, "読み込めないこと", &pt, mode);
  
  sp("\n■使用するパスワードを passwdGood に変更する");
  nfc.setAuthKey(&passwdGood);    // 正しいパスワード

  sp("\n■モード変更  パスワード=passwdDefault  PRT_NOPASS_RW");
  res = nfc.writeProtect(PRT_NOPASS_RW, &passwdDefault, vindex, tnum, mode);
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■(2)で書いたデータが残っているか？");
  subTest(vindex, 11, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■通常の書き込み(4)");
  subTest(vindex, 44, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 44, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  // デフォルトに戻す
  nfc.setAuthKey(&passwdDefault);    // デフォルトパスワード

  // 最終判定
  res = (pt.point == pt.total);
  spf("\n================\n■最終判定 %s (%d/%d)\n================\n", tf(res), pt.point, pt.total);
  return res;
}

// PRT_PASSWD_RO : PW認証あり・読み込み専用モード
bool testPasswdReadonly() {
  bool res;
  Points pt;
  uint16_t vindex = 0;
  const size_t tnum = (nfc.isClassic()) ? 32 : 16;
  ULConfigEx ulconfex;
  ProtectMode mode;
  
  sp("\n\n\n**** PRT_PASSWD_RO : PW認証あり・読み込み専用モード : PW認証で保護されていて書き込めないことを確認します ****");
  mode = PRT_NOPASS_RW;

  sp("\n■通常の書き込み(1)");
  subTest(vindex, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■モード変更  パスワード=passwdGood  PRT_PASSWD_RO");
  res = nfc.writeProtect(PRT_PASSWD_RO, &passwdGood, vindex, tnum, mode);
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■使用するパスワードを passwdGood に変更する");
  nfc.setAuthKey(&passwdGood);    // 正しいパスワード

  sp("\n■正規PWで書き込み(2)");
  subTest(vindex, 11, tnum, true, TEST_WRITE, "書き込めないこと", &pt, mode);

  sp("\n■(1)で書いたデータが残っているか？");
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■KeyBで要認証のときにKeyAで読み書きができないこと（NFCはプロテクト状態のまま）");
  subTest(vindex, 33, tnum, true, TEST_WRITE, "書き込めないこと", &pt, PRT_NOPASS_RW);
  subTest(vindex, 33, tnum, true, TEST_READ, "読み込めないこと", &pt, PRT_NOPASS_RW);

  sp("\n■使用するパスワードを passwdBad に変更する");
  nfc.setAuthKey(&passwdBad);    // 不正なパスワード

  sp("\n■不正PWで書き込めないはず(3)");
  subTest(vindex, 22, tnum, true, TEST_WRITE, "書き込めないこと", &pt, mode);
  subTest(vindex, 22, tnum, true, TEST_READ, "読み込めないこと", &pt, mode);

  sp("\n■使用するパスワードを passwdGood に変更する");
  nfc.setAuthKey(&passwdGood);    // 正しいパスワード

  sp("\n■モード変更  パスワード=passwdDefault  PRT_NOPASS_RW");
  res = nfc.writeProtect(PRT_NOPASS_RW, &passwdDefault, vindex, tnum, mode);
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■(1)で書いたデータが残っているか？");
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  // デフォルトに戻す
  nfc.setAuthKey(&passwdDefault);    // デフォルトパスワード

  // 最終判定
  res = (pt.point == pt.total);
  spf("\n================\n■最終判定 %s (%d/%d)\n================\n", tf(res), pt.point, pt.total);
  return res;
}

// [共通] 全ての領域の読み書きをチェック
bool testAllArea(uint16_t totalSize) {
  bool res;
  bool classic = nfc.isClassic();
  Points pt;
  uint16_t vindex = 0;
  const size_t tnum = totalSize;
  ProtectMode mode = PRT_AUTO;

  sp("\n\n\n**** 全ての領域の読み書きができるかチェックします ****");
  sp("\n■通常の書き込みテスト");
  subTest(vindex, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  // 最終判定
  res = (pt.point == pt.total);
  spf("\n================\n■最終判定 %s (%d/%d)\n================\n", tf(res), pt.point, pt.total);
  return res;
}

// テスト
bool testPasswdUL(ProtectMode testmode) {
  bool res;
  bool classic = nfc.isClassic();
  Points pt;
  uint16_t vindex = 0;
  const size_t tnum = (classic) ? 32 : 16;
  ULConfigEx ulconfex;
  ProtectMode mode;
  
  String modeText;
  if (testmode == PRT_PASSWD_RW) {
    sp("\n\n\n**** PRT_PASSWD_RW : PW認証あり・読み書きモード : PW認証で保護されていることを確認します ****");
    modeText = "PRT_PASSWD_RW";
  } else if (testmode == PRT_PASSWD_RO) {
      sp("\n\n\n**** PRT_PASSWD_RO : PW認証あり・読み込み専用モード : PW認証で保護されていて書き込めないことを確認します ****");
      modeText = "PRT_PASSWD_RO";
  } else {
    return false;
  }
  mode = PRT_NOPASS_RW;
  nfc._authedUL = true;

  sp("\n■1 パスワード認証を無効化（元々してないが念のため）");
  res = nfc.unauthUL();
  spp("unauthUL()", tf(res));
  judge(res, &pt);

  sp("\n■2 通常の書き込み");
  subTest(vindex, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);
  subTest(vindex+64, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex+64, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■3 モード変更  パスワード=passwdGood  "+modeText);
  res = nfc.writeProtect(testmode, &passwdGood, 16, 0, mode);
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■4 リセット unauthUL()の実行");
  res = nfc.unauthUL();
  spp("unauthUL()", tf(res));
  judge(res, &pt);

  sp("\n■5 書き込み(1)");
  subTest(vindex, 11, tnum, false, TEST_WRITE, "書き込み", &pt, PRT_NOPASS_RW);
  subTest(vindex, 11, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, PRT_NOPASS_RW);

  sp("\n■6 !書き込み(2) できないはず");
  subTest(vindex+64, 11, tnum, true, TEST_WRITE, "!書き込み", &pt, PRT_NOPASS_RW);
  if (testmode == PRT_PASSWD_RW) {
    subTest(vindex+64, 0, tnum, true, TEST_READ, "!読み込み", &pt, PRT_NOPASS_RW);
    subTest(vindex+64, 0, tnum, true, TEST_READ_COMPARE, "!読み込み+比較", &pt, PRT_NOPASS_RW);
  } else if (testmode == PRT_PASSWD_RO) {
    subTest(vindex+64, 0, tnum, false, TEST_READ, "読み込み", &pt, PRT_NOPASS_RW);
    subTest(vindex+64, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, PRT_NOPASS_RW);
  }

  sp("\n■7 使用するパスワードを passwdGood に変更する");
  nfc.setAuthKey(&passwdGood);    // 正しいパスワード

  // sp("■9 パスワード認証する passwdGood");
  // res = nfc.authUL(true, true);  // DEBUG: PACKチェックする、強制的に認証する
  // spp("authUL()", tf(res));
  // judge(res, &pt);

  sp("\n■8 設定読み込み");
  res = showConfigInfo(&ulconfex, true, mode);   // [Ultralight] 設定情報の読み込み
  spp("showConfigInfo()", tf(res));
  judge(res, &pt);

  sp("\n■9 書き込み(1)");
  subTest(vindex, 22, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 22, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■10 書き込み(2) 今度はできるはず");
  subTest(vindex+64, 22, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex+64, 22, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■11 モード変更  パスワード=nullptr  PRT_NOPASS_RW");
  // res = nfc.writeProtect(PRT_NOPASS_RW, nullptr, vindex, 0, mode);
  res = nfc.writeProtectUL(PRT_NOPASS_RW, nullptr, 255, true, mode);  // 物理アドレス指定
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■12 リセット unauthUL()の実行 パスワード認証を無効化");
  res = nfc.unauthUL();
  spp("unauthUL()", tf(res));
  judge(res, &pt);

  sp("\n■13 通常の書き込み");
  subTest(vindex, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);
  subTest(vindex+64, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex+64, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  // 最終判定
  res = (pt.point == pt.total);
  spf("================\n■最終判定 %s (%d/%d)\n================\n", tf(res), pt.point, pt.total);
  return res;
}

//
// テストルーチン（その他） ------------------------------------------------------
//

// カード情報を表示する
void utilShowCardInfo() {
  sp("\n\n\n**** カード情報 ****");
  spf("UID: %s\n", nfc.getUidString().c_str());
  if (nfc.isClassic()) {
    sp("カード種別: MIFARE Classic");
    spf("使用可能セクタ範囲: %d～%d\n", nfc._minSectorCL, nfc._maxSectorCL);
  } else if (nfc.isUltralight()) {
    // NTAG種別
    spn("カード種別: MIFARE Ultralight ");
    if (nfc._ntagType == NT_NTAG213) sp("NTAG213");
    else if (nfc._ntagType == NT_NTAG215) sp("NTAG215");
    else if (nfc._ntagType == NT_NTAG216) sp("NTAG216");
    else sp("unknown");
    spf("使用可能ページ範囲: %d～%d\n", nfc._minPageUL, nfc._maxPageUL);
    spf("設定ページ: %d\n", nfc._configPageUL);
  } else {
    sp("無効なカード");
  }
  uint16_t totalSize = nfc.getVCapacities();  // 使用可能な容量（仮想アドレス換算）を取得する
  spf("使用可能な容量: %d バイト\n", totalSize);
}
void utilShowCardInfoEx() {
  utilShowCardInfo();
  sp("");
  if (nfc.isUltralight()) {
    ULConfigEx ulconfex;
    showConfigInfo(&ulconfex, true);
  }
}

// メモリダンプ
void testMemoryDump() {
  sp("\n\n\n**** 全てのデータを取得します ****");
  // nfc._dbgopt |= NFCOPT_DUMP_AUTHFAIL_CONTINUE;
  nfc.dumpAll();
  nfc.unmountCard();
  nfc.mountCard(5000);
  if (nfc.isUltralight()) {
    ULConfigEx ulconfex;
    showConfigInfo(&ulconfex, true);
  }
}

// リセット
void utilReset() {
  sp("\n\n\n**** 再起動します ****");
  delay(1000);
  nfc.unmountCard();
  ESP.restart();
}

// セクタートレーラー(設定情報)の修復
void developRecover() {
  if (nfc.isClassic()) {
    recoverCardCL();   
  } else if (nfc.isUltralight()) {
    recoverCardUL();
  }
  nfc.unmountCard();
  nfc.mountCard(5000);
}

// フォーマット
void utilFormat() {
  sp("\n\n\n**** フォーマットします ****");
  delay(1000);
  nfc.format(true);
}

// 実験 [Ultralight] LIMIT値を超える認証失敗でロックされるかどうかのテスト群
void developAuthfailLimit() {
  if (! nfc.isUltralight()) return;
  ULConfig ulconf;
  const byte failLimit = 7;  // 0 or 1-7
  bool res;
  sp("認証失敗リミットを設定する");
  if (nfc.readConfigDataUL(&ulconf)) {
    ulconf.ACCESS = (ulconf.ACCESS & ~0x07) | (failLimit & 0x07);   // AUTHLIM
    res = nfc.writeConfigDataUL(&ulconf);   // 一旦AUTHLIMのみ変更
    spp("writeConfigDataUL()", tf(res));
    if (res) {  // モード変更（パスワード、ページ制限）
      res = nfc.writeProtect(PRT_PASSWD_RW, &passwdGood, 16, 0, PRT_NOPASS_RW);
      spp("writeProtect()", tf(res));
    }
    sp("以下、変更後の設定情報 ----");
    nfc.setAuthKey(&passwdGood);    // 正しいパスワード
    ULConfigEx ulconfex;
    showConfigInfo(&ulconfex, true);
  }
  res = nfc.unauthUL();
  spp("unauthUL()", tf(res));
  return;
}
void developAuthfailLimit_test1() {
  sp("非プロテクトエリアへのアクセス PWなし");
  bool res = subTest(0, 0, 16, false, TEST_READ, "読み込み", nullptr, PRT_NOPASS_RW);
  spp("読み込み", tf(res));
}
void developAuthfailLimit_test2() {
  sp("プロテクトエリアへのアクセス PWなし");
  bool res = subTest(32, 0, 16, false, TEST_READ, "読み込み", nullptr, PRT_NOPASS_RW);
  spp("読み込み", tf(res));
}
void developAuthfailLimit_test3() {
  sp("プロテクトエリアへのアクセス PWあり (passwdGood)");
  nfc.setAuthKey(&passwdGood);    // 正しいパスワード
  bool res = subTest(32, 0, 16, false, TEST_READ, "読み込み", nullptr, PRT_PASSWD_RW);
  nfc.unauthUL();
  spp("読み込み", tf(res));
}
void developAuthfailLimit_test4() {
  sp("プロテクトエリアへのアクセス PWあり (passwdBad)");
  nfc.setAuthKey(&passwdBad);    // 正しいパスワード
  bool res = subTest(32, 0, 16, false, TEST_READ, "読み込み", nullptr, PRT_PASSWD_RW);
  nfc.unauthUL();
  spp("読み込み", tf(res));
}

// 実験 [Ultralight] page.0からプロテクトをかけたらどうなるか？
void developPage0Protect() {
  if (! nfc.isUltralight()) return;
  ProtectMode mode;
  bool res;
  sp("page.0からプロテクトをかける");
  if (nfc.writeProtectUL(PRT_PASSWD_RW, &passwdGood, 0, true, PRT_NOPASS_RW)) {
    mode = nfc._lastProtectMode;
    sp("以下、変更後の設定情報 ----");
    nfc.setAuthKey(&passwdGood);    // 正しいパスワード
    ULConfigEx ulconfex;
    showConfigInfo(&ulconfex, true, mode);  // 第3引数は無くてもモードは自動判定される
  }
  res = nfc.unauthUL(mode);   // 再マウント時にモード指定が必要なので指定する（_lastProtectModeがクリアされてしまうから）
  spp("unauthUL()", tf(res));
  return;
}
void developPage0Protect_mount() {
  ProtectMode mode = PRT_PASSWD_RW;
  Points point;
  bool res;
  sp("page.0からプロテクトがかかったカードをマウントする");
  nfc.setAuthKey(&passwdGood);    // 正しいパスワード
  res = nfc.mountCard(5000, mode);
  spp("mountCard()", tf(res));
  res = subTest(0, 0, 16, false, TEST_READ, "読み込み", &point, mode);
}

// アンテナゲイン関連
void developGetAntennaGain() {
  byte gain = nfc.mfrc522.PCD_GetAntennaGain();
  spp("PCD_GetAntennaGain", gain);
}
void developSetAntennaGain() {
  byte gain = MFRC522_I2C::RxGain_max;  // max 48 dB, (default 33 dB)
  nfc.mfrc522.PCD_SetAntennaGain(gain);
  spp("PCD_SetAntennaGain", gain);
}

// セルフテスト
void developSelfTest() {
  bool res = nfc.firmwareVersionCheck();
  spp("firmwareVersionCheck()", tf(res));
}

// プロテクトされたDumpAll
void develoProtectedDump() {
  bool res;
  Points pt;
  uint16_t vindex  = 0;
  size_t   tnum    = (nfc.isClassic()) ? 48*4 : 16*4;
  uint16_t pvindex = (nfc.isClassic()) ? 48 : 16;
  size_t   ptnum   = (nfc.isClassic()) ? 48*2 : 16*2;
  ProtectMode mode;

  sp("\n\n\n**** プロテクトされたカードの全てのデータを取得します ****");
  mode = PRT_NOPASS_RW;

  sp("\n■通常の書き込みテスト");
  subTest(vindex, 0, tnum, false, TEST_WRITE, "書き込み", &pt, mode);
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  sp("\n■モード変更  パスワード=passwdGood  PRT_PASSWD_RW");
  if (nfc.isClassic()) {
    res = nfc.writeProtect(PRT_PASSWD_RW, &passwdGood, pvindex, ptnum, mode);
  } else if (nfc.isUltralight()) {
    res = nfc.writeProtect(PRT_PASSWD_RW, &passwdGood, pvindex, ptnum, mode);
  }
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■使用するパスワードを passwdGood に変更する");
  nfc.setAuthKey(&passwdGood);    // 正しいパスワード

  // ダンプ
  // sp("\n■ダンプします - 読み込めないことを目視で確認する");
  // nfc.dumpAll()

  // ダンプ
  sp("\n■ダンプします - 読み込めることを目視で確認する");
  PhyAddr pa1 = nfc.addr2PhysicalAddr(pvindex, nfc._cardType);
  PhyAddr pa2 = nfc.addr2PhysicalAddr(pvindex + ptnum - (nfc.isClassic() ? nfc._writeLengthCL : nfc._writeLengthUL), nfc._cardType);
  spf("dumpAll(true, %d, %d)\n", pa1.blockAddr, pa2.blockAddr);
  nfc.dumpAll(true, pa1.blockAddr, pa2.blockAddr);

  // 再マウント
  sp("\n■再マウントします");
  nfc.unmountCard();
  nfc.mountCard(5000);
  nfc._lastProtectMode = mode;

  sp("\n■モード変更  パスワード=passwdDefault  PRT_NOPASS_RW");
  if (nfc.isClassic()) {
    res = nfc.writeProtect(PRT_NOPASS_RW, &passwdDefault, pvindex, ptnum, mode);
  } else if (nfc.isUltralight()) {
    res = nfc.writeProtect(PRT_NOPASS_RW, &passwdDefault, 10000, -1, mode);
  }
  mode = nfc._lastProtectMode;
  spp("writeProtect()", tf(res));
  judge(res, &pt);

  sp("\n■通常の読み込みテスト");
  subTest(vindex, 0, tnum, false, TEST_READ_COMPARE, "読み込み+比較", &pt, mode);

  // デフォルトに戻す
  nfc.setAuthKey(&passwdDefault);    // デフォルトパスワード

  // 最終判定
  res = (pt.point == pt.total);
  spf("\n================\n■最終判定 %s (%d/%d)\n================\n", tf(res), pt.point, pt.total);
}

//
// テストルーチン（総合） ------------------------------------------------------
//

// テスト開始（簡易テスト）
void testStartEasy() {
  judge(testArrayData(), nullptr);  // [共通] 基本テスト　配列のデータを読み書き
}

// テスト開始（フルテスト）
void testStart() {
  bool res = false;
  Points pt;

  // 基本テスト
  judge(testArrayData(), &pt);  // [共通] 基本テスト　配列のデータを読み書き
  judge(testStructData(), &pt); // [共通] 基本テスト　構造体のデータを読み書き

  // 応用テスト
  judge(testNopassReadwrite(), &pt);   // PRT_NOPASS_RW : PW認証なし・読み書きモード
  if (nfc.isClassic()) {
    judge(testNopassReadonly(), &pt);    // PRT_NOPASS_RO : PW認証なし・読み込み専用モード
    judge(testPasswdReadwrite(), &pt);   // PRT_PASSWD_RW : PW認証あり・読み書きモード
    judge(testPasswdReadonly(), &pt);    // PRT_PASSWD_RO : PW認証あり・読み込み専用モード
  } else if (nfc.isUltralight()) {
    judge(testPasswdUL(PRT_PASSWD_RW), &pt);    // PRT_PASSWD_RW : PW認証あり・読み書きモード
    judge(testPasswdUL(PRT_PASSWD_RO), &pt);    // PRT_PASSWD_RO : PW認証あり・読み込み専用モード
  }

  // 全領域テスト
  // uint16_t totalSize = nfc.getVCapacities();  // 使用可能な容量（仮想アドレス換算）を取得する
  // judge(testAllArea(totalSize), &pt);  // [共通] 全ての領域の読み書きをチェック  

  // 最終判定
  res = (pt.point == pt.total);
  spf("\n\n\n**** テスト終了 (%d/%d) ****\n", pt.point, pt.total);
  sp(res ? "全てのテストに合格しました\n\n" : "不合格のテストがあります\n\n");
}

//
// メインルーチン ------------------------------------------------------
//

// メニューリスト
struct MenuList {
  String title;
  int16_t id;
  void (*function)();
};
MenuList menuList[] = {
  { "カード情報", 0, utilShowCardInfoEx }, 
  { "テスト開始（簡易テスト）", 0, testStartEasy }, 
  { "テスト開始（フルテスト）", 0, testStart }, 
  { "メモリダンプ", 0, testMemoryDump }, 
  { "実験 develoProtectedDump", 0, develoProtectedDump }, 
  { "ESPリセット", 0, utilReset }, 
  { "拡張メニュー表示", 99, nullptr }, 
  { "NDEF書込済Mifare Classic対応モード", 101, nullptr }, 
  { "セクタートレーラー(設定情報)の修復", 0, developRecover }, 
  { "フォーマット", 0, utilFormat }, 
  { "デバッグ情報の切り替え", 102, nullptr }, 
};
const int menuCount = sizeof(menuList) / sizeof(menuList[0]);

// 初期設定 ------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  // I2Cの設定
  int8_t pinSda, pinScl;
  switch(M5.getBoard()) {
    case m5gfx::board_t::board_M5Dial:  // M5Dialは内部I2C
      pinSda = M5.getPin(m5::pin_name_t::in_i2c_sda);
      pinScl = M5.getPin(m5::pin_name_t::in_i2c_scl);
      break;
    default:  // その他はPORT A
      pinSda = M5.getPin(m5::pin_name_t::port_a_sda);
      pinScl = M5.getPin(m5::pin_name_t::port_a_scl);
    break;
  }
  spf("I2C Pin Setting SDA=%d SCL=%d\n", pinSda, pinScl);
  Wire.begin(pinSda, pinScl);
  delay(500);

  // カードの認識
  nfc.init();
  nfc.mfrc522.PCD_SetAntennaGain(MFRC522_I2C::RxGain_max);   // 48dB (default 33dB)
  nfc._debug = true;
  sp("\n\n\n**** 初期化 ****");
  spn("カードを置いてください");
  while (!nfc.mountCard(200)) {
    M5.update();
    spn(".");
    if (M5.BtnA.wasPressed()) break;
  }
  sp("\nカードを認識しました");
  delay(500);
  utilShowCardInfo();   // カード情報を表示する
}

// メイン ------------------------------------------------------
void loop() {
  int menu;
  static bool exMenuMode = false;

  // メニューを表示
  sp("\n\n\n【メニュー】");
  for (int i=0; i<menuCount; i++) {
    spf("%d. %s\n", i+1, menuList[i].title.c_str());
    if (menuList[i].id == 99 && !exMenuMode) break;
  }
  spn("入力してください>> ");
  while (!Serial.available()) delay(10);
  String input = Serial.readStringUntil('\n');
  input.trim();
  menu = input.toInt();
  sp(menu);
  sp("\n\n");

  // メニュー別の処理
  if (menu < 1 || menu > menuCount) return;
  if (menuList[menu-1].function != nullptr) {
    menuList[menu-1].function();  // 関数が指定されていれば関数を実行
    return;
  }
  switch (menuList[menu-1].id) {
    case 99:
      exMenuMode = true;
      break;
    case 101:
      sp("dumpAll()でNDEF書き込み済のMifare Classicを読めるようにする");
      nfc._dbgopt |= NFCOPT_DUMP_NDEF_CLASSIC | NFCOPT_DUMP_AUTHFAIL_CONTINUE;
      break;
    case 102:
      nfc._debug = !nfc._debug;
      sp("デバッグ情報の出力設定: "+ String((nfc._debug) ? "ON" : "OFF"));
      break;
  }

  delay(50);
}

