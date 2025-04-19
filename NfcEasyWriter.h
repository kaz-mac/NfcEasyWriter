/*
  NfcEasyWriter.h
  NFCカードを簡単に読み書きするためのクラス Ver.1.2

  https://github.com/kaz-mac/NfcEasyWriter

  想定するカード: MIFARE Classic 1K, NTAG213/215/216
  想定するリーダー: M5Stack RFID 2 Unit (WS1850S)
  必要なライブラリ: MFRC522_I2C  https://github.com/kkloesener/MFRC522_I2C

  Copyright (c) 2025 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT
*/
/*
  以下、参考資料

  M5Stack用WS1850S搭載 RFID 2ユニット
  https://docs.m5stack.com/en/unit/rfid2
  https://www.switch-science.com/products/8301

  M5Stack版MFRC522_I2Cのサンプル
  https://github.com/m5stack/M5Stack/tree/master/examples/Unit/RFID_RC522

  参考ページ
  sample https://logikara.blog/rfid/

  データシート
  MIFARE Classic 1K https://orangetags.com/rfid-chip-datasheet/nxp-rfid-chip-datasheet/mifare/nxp-mifare-1k-datasheet/
  NTAG 213/215/216 https://www.nxp.jp/products/NTAG213_215_216
  MIFARE Classic NDEF format https://www.nxp.com/docs/en/application-note/AN1305.pdf
*/
#pragma once
#include <MFRC522_I2C.h>

// デバッグに便利なマクロ定義 --------
#define sp(x) Serial.println(x)
#define spn(x) Serial.print(x)
#define spf(fmt, ...) Serial.printf(fmt, __VA_ARGS__)
#define spp(k,v) Serial.println(String(k)+"="+String(v))
#define array_length(x) (sizeof(x) / sizeof(x[0]))

// オプション
#define NFCOPT_DUMP_NDEF_CLASSIC        1  // dumpAll()でNDEF書き込み済のMifare Classicを読む（NFC Toolsで書き込んだデータを見るときに使う）
#define NFCOPT_DUMP_AUTHFAIL_CONTINUE   2  // dumpAll()でClassicの認証エラーが出ても続行する
#define NFCOPT_DUMP_UL255PAGE_READ      4  // dumpAll()で強制的にUltralightのpage=255まで読む

// 各種定義
enum CardType : uint8_t { UnknownCard, Classic, Ultralight };   // カードの種類
enum NtagType : uint8_t { NT_UNKNOWN, NT_NTAG213, NT_NTAG215, NT_NTAG216 };   // 容量(Ultralight)
enum ProtectMode : uint8_t {  // プロテクトモード
  PRT_AUTO,         // 最後にwriteProtect()設定した値　※これは明示的に使用するものではない
  PRT_NOPASS_RW,    // パスワード認証:なし、読み書き可  （KeyAで読み込み可 KeyAで書き込み可）
  PRT_NOPASS_RO,    // パスワード認証:なし、読み込み専用（KeyAで読み込み可 書き込み不可）
  PRT_PASSWD_RW,    // パスワード認証:あり、読み書き可  （KeyBで読み込み可 KeyBで書き込み可）
  PRT_PASSWD_RO,    // パスワード認証:あり、読み込み専用（KeyBで読み込み可 書き込み不可）
};
struct PhyAddr {  // 物理アドレス
  uint16_t sector;
  uint16_t block;
  uint16_t blockAddr;
};
struct ULConfig { // Ultralightの設定情報 16バイト
  byte MIRROR, RFUI0, MIRROR_PAGGE, AUTH0;
  byte ACCESS, RFUI1, RFUI2, RFUI3;
  byte PWD4[4];
  byte PACK[2], RFUI4, RFUI5;
};
struct ULConfigEx { // Ultralightの設定情報＋参考値
  ULConfig ulconf;
  bool PROT, CFGLCK;
  byte AUTHLIM;
};
struct AuthKey {  // 認証キー（Classicは48bit使用、Ultralightは32bit使用）
  byte keyByte[6];
};


//
// 派生クラスで新しい機能を追加
//
class MFRC522_I2C_Extend : public MFRC522_I2C {
public:
  MFRC522_I2C_Extend(byte chipAddress, byte resetPowerDownPin, TwoWire *TwoWireInstance = &Wire)
    : MFRC522_I2C(chipAddress, resetPowerDownPin, TwoWireInstance) {}
  // MFRC522の初期化（MFRC522_I2CのPCD_Init()からリセットピンのGPIOの動作を除いたもの）
  void PCD_Init_without_resetpin();
  // Mifare Ultralightのパスワード認証を行う
  byte MIFARE_Ultralight_Authenticate(byte* password, byte* passwordLen, byte* pack, byte* packLen);
};


//
// NFCカードを簡単に読み書きするためのクラス
//
class NfcEasyWriter {
public:
  MFRC522_I2C_Extend& mfrc522;  // MFRC522_I2C オブジェクトの参照を保持
  bool _debug = false;  // Serialにデバッグ出力
  uint16_t _dbgopt = 0;   // デバッグオプション
  uint16_t _minSectorCL = 1;   // Classicで使用するセクタの先頭
  uint16_t _maxSectorCL = 15;  // Classicで使用するセクタの最後
  uint16_t _minPageUL = 5;     // Ultralightで使用するページの先頭（4以上指定可）
  uint16_t _maxPageUL = 39;    // Ultralightで使用するページの最後 39/129/225
  uint16_t _configPageUL = 0;  // Ultralightの設定ページ 41/131/227
  const uint16_t _writeLengthCL = 16;  // Classicの書き込み単位
  const uint16_t _writeLengthUL = 4;   // Ultralightの書き込み単位
  const uint16_t _readLength = 16;     // CL/UL共通の読み込み単位
  MFRC522_I2C::MIFARE_Key _authKeyA = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // デフォルトの認証用（通常は変更しない）
  MFRC522_I2C::MIFARE_Key _authKeyB = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // プロテクト時の認証用
  MFRC522_I2C::MIFARE_Key _authKeyBDefault = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // KeyBのデフォルト値（プロテクト解除時に使う）
  MFRC522_I2C::MIFARE_Key _authKeyNdefClassic0 = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5 };  // NDEF書込済Classicの初期値 sector0
  MFRC522_I2C::MIFARE_Key _authKeyNdefClassic1 = { 0xD3, 0xF7, 0xD3, 0xF7, 0xD3, 0xF7 };  // NDEF書込済Classicの初期値 sector1以降
  bool _authedUL = true;    // 認証済みフラグ
  ProtectMode _lastProtectMode = PRT_NOPASS_RW;  // 最後に設定したプロテクトモード 内部参照用

  // マウント時のカード情報
  bool _mounted = false;
  CardType _cardType = UnknownCard;
  NtagType _ntagType = NT_UNKNOWN;

  // コンストラクタ　MFRC522_I2C の参照を受け取る
  NfcEasyWriter(MFRC522_I2C_Extend& ref) : mfrc522(ref) {}

  // 初期化
  void init();

  // 読み書きできる状態になるまで待つ
  bool waitCard(uint32_t timeout=5000);

  // カードをマウントする（読み書きできる状態になるまで待つ）
  bool mountCard(uint32_t timeout=0, ProtectMode mode=PRT_AUTO);

  // カードのマウントを解除する
  void unmountCard();

  // UIDを文字列で返す
  String getUidString();

  // カードの種類を大まかに判定する
  CardType checkCardType(MFRC522_I2C &mfrc522);

  // カードがマウントされているか？（mountCard()が成功したか見てるだけ）
  bool isMounted();

  // カードの種類 Mifare Classicか？
  bool isClassic();

  // カードの種類 Mifare Ultralightか？
  bool isUltralight();

  // [Ultralight] NTAGの容量タイプを取得する
  NtagType getNtagTypeUL(ProtectMode mode=PRT_AUTO);

  // [Ultralight]  書き込み可能なページ最大値を取得する
  uint8_t getMaxPageUL(NtagType ntag);

  // [Ultralight] の設定ページのページ位置を取得する
  uint8_t getConfigPageUL(NtagType ntag);

  // 使用可能な容量（仮想アドレス換算）を取得する
  uint16_t getVCapacities();

  // [Ultralight] 物理アドレス指定　4ページ(16バイト)読み込む
  bool rawReadUL(byte* data, size_t dataSize, uint8_t page);

  // [Ultralight] 物理アドレス指定　1ページ(4バイト)書き込む
  bool rawWriteUL(byte* data, size_t dataSize, uint8_t page);

  // 仮想アドレスから物理アドレスに変換する
  PhyAddr addr2PhysicalAddr(uint16_t vaddr, CardType cardtype);

  // カードからデータを読み込む
  bool readData(uint16_t vaddr, void* data, size_t dataSize, ProtectMode mode=PRT_AUTO);    // 共通
  bool readDataCL(uint16_t vaddr, byte* data, size_t dataSize, ProtectMode mode=PRT_AUTO);  // for Classic
  bool readDataUL(uint16_t vaddr, byte* data, size_t dataSize, ProtectMode mode=PRT_AUTO);  // for Ultralight

  // データをカードに書き込む
  bool writeData(uint16_t vaddr, void* data, size_t dataSize, ProtectMode mode=PRT_AUTO);    // 共通
  bool writeDataCL(uint16_t vaddr, byte* data, size_t dataSize, ProtectMode mode=PRT_AUTO);  // for Classic
  bool writeDataUL(uint16_t vaddr, byte* data, size_t dataSize, ProtectMode mode=PRT_AUTO);  // for Ultralight

  // 認証キーを設定する（書き込みはしない）
  void setAuthKey(AuthKey* key);
  void setAuthKey(MFRC522_I2C::MIFARE_Key* key);

  // プロテクトモード未指定時(PRT_AUTO)のデフォルト動作を指定する（書き込みはしない）
  void setNowProtectMode(ProtectMode mode);

  // プロテクトモードや認証キーを書き込む（Classicは指定した仮想アドレスの範囲にあるセクター全て、Ultralightは以降全て）
  bool writeProtect(ProtectMode mode, AuthKey* key, uint16_t vaddr, int size, ProtectMode lastmode=PRT_AUTO);   // 共通
  bool writeProtectCL(ProtectMode mode, AuthKey* key, uint16_t vaddr, int size, ProtectMode lastmode=PRT_AUTO); // Classic
  bool writeProtectUL(ProtectMode mode, AuthKey* key, uint16_t vaddr, bool phyaddr=false, ProtectMode lastmode=PRT_AUTO); // Ultralight

  // [Ultralight] パスワード認証を行う
  bool authUL(bool checkPack=true);

  // [Ultralight] パスワード認証を無効化にする（アンマウントしてから再マウントする）
  bool unauthUL(ProtectMode mode=PRT_AUTO);

  // [Ultralight] 設定情報を取得する
  bool readConfigDataUL(ULConfig* ulconf, ProtectMode mode=PRT_AUTO);
  bool readConfigDataUL(ULConfigEx* ulconfex, ProtectMode mode=PRT_AUTO);

  // [Ultralight] 設定情報を書き込む
  bool writeConfigDataUL(ULConfig* ulconf, ProtectMode mode=PRT_AUTO);

  // データ領域のフォーマット（NDEFメッセージを削除する）
  bool format(bool formatAll);

  // ファームウェアバージョンのチェック
  bool firmwareVersionCheck();

  //
  // 以下はデバッグ用 -----------------------------------------------------------------
  //

  // 壊してしまったセクタートレーラー(設定情報)を修復する
  bool recoverySectorTruckCL(uint16_t blockAddr, AuthKey* key, bool useKeyB=true);
  bool recoveryConfigDataUL(bool useAuth, AuthKey* key, ProtectMode lastmode=PRT_AUTO);

  // 全データをシリアルに出力する　デバッグ用　（MFRC522_I2Cライブラリ標準のdump結果）
  void dumpAllBasic();

  // 全データをシリアルに出力する　デバッグ用　（読めるところまで読んでみる）
  void dumpAll(bool inProtect=false, uint8_t phySta=255, uint8_t phyEnd=255);

  // バイナリのdumpを出力する 16進数表示
  void printDump(const byte *data, size_t dataSize, String sepa="-", String cr="\n", String crend="\n");
  void printDump1Line(const byte *data, size_t dataSize);

  // 8桁の2進数を返す
  String dec2bin8(uint8_t num);
  // バイナリのdumpを出力する 2進数表示
  void printDumpBin(const byte *data, size_t dataSize);

};
