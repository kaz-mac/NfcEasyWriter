/*
  NfcEasyWriter.cpp
  NFCカードを簡単に読み書きするためのクラス Ver.1.2

  https://github.com/kaz-mac/NfcEasyWriter

  想定するカード: MIFARE Classic 1K, NTAG213/215/216
  想定するリーダー: M5Stack RFID 2 Unit (WS1850S)
  必要なライブラリ: MFRC522_I2C  https://github.com/kkloesener/MFRC522_I2C

  Copyright (c) 2025 Kaz  (https://akibabara.com/blog/)
  Released under the MIT license.
  see https://opensource.org/licenses/MIT
*/
#include "NfcEasyWriter.h"

// MFRC522の初期化（MFRC522_I2CのPCD_Init()からリセットピンのGPIOの動作を除いたもの）
void MFRC522_I2C_Extend::PCD_Init_without_resetpin() {
  // Perform a soft reset
  PCD_Reset();
	// When communicating with a PICC we need a timeout if something goes wrong.
	// f_timer = 13.56 MHz / (2*TPreScaler+1) where TPreScaler = [TPrescaler_Hi:TPrescaler_Lo].
	// TPrescaler_Hi are the four low bits in TModeReg. TPrescaler_Lo is TPrescalerReg.
	PCD_WriteRegister(TModeReg, 0x80);			// TAuto=1; timer starts automatically at the end of the transmission in all communication modes at all speeds
	PCD_WriteRegister(TPrescalerReg, 0xA9);		// TPreScaler = TModeReg[3..0]:TPrescalerReg, ie 0x0A9 = 169 => f_timer=40kHz, ie a timer period of 25�s.
	PCD_WriteRegister(TReloadRegH, 0x03);		// Reload timer with 0x3E8 = 1000, ie 25ms before timeout.
	PCD_WriteRegister(TReloadRegL, 0xE8);

	PCD_WriteRegister(TxASKReg, 0x40);		// Default 0x00. Force a 100 % ASK modulation independent of the ModGsPReg register setting
	PCD_WriteRegister(ModeReg, 0x3D);		// Default 0x3F. Set the preset value for the CRC coprocessor for the CalcCRC command to 0x6363 (ISO 14443-3 part 6.2.4)
	PCD_AntennaOn();						// Enable the antenna driver pins TX1 and TX2 (they were disabled by the reset)
}

// Mifare Ultralightのパスワード認証を行う
byte MFRC522_I2C_Extend::MIFARE_Ultralight_Authenticate(byte* password, byte* passwordLen, byte* pack, byte* packLen) {
	// Sanity check
	if (password == NULL || *passwordLen != 4 || pack == NULL || *packLen != 4) {
		return STATUS_ERROR;
	}

	// Build command buffer
  byte command[7] = {0};
  command[0] = 0x1B; // PWD_AUTH command
  memcpy(&command[1], password, 4);

	// Calculate CRC_A
	byte result = PCD_CalculateCRC(command, 5, &command[5]);
	if (result != STATUS_OK) {
		return result;
	}

	// Transmit the buffer and receive the response, validate CRC_A.
  return PCD_TransceiveData(command, sizeof(command), pack, packLen, NULL, 0, true);
}


// 初期化
void NfcEasyWriter::init() {
  mfrc522.PCD_Init_without_resetpin();   // RFID2（MFRC522）初期化
}

// 読み書きできる状態になるまで待つ
bool NfcEasyWriter::waitCard(uint32_t timeout) {
  uint32_t tm = millis() + timeout;
  bool stat = false;
  while (!stat) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      stat = true;
      break;
    } else if (timeout > 0 && tm < millis()) {
      break;
    }
    delay(100);
  }
  return stat;
}

// カードをマウントする（読み書きできる状態になるまで待つ）
bool NfcEasyWriter::mountCard(uint32_t timeout, ProtectMode mode) {
  bool stat;

  // マウント中なら先にアンマウントする
  if (_mounted) {
    unmountCard();  // アンマウント
  }

  // カード情報を取得する
  init();
  stat = waitCard(timeout);  // 読み書きできる状態になるまで待つ
  if (stat) {
    _cardType = checkCardType(mfrc522);
    if (_cardType == CardType::Classic) {
      _mounted = true;
      if (_debug) sp("Mifare Classic mounted");
    } else if (_cardType == CardType::Ultralight) {
      _ntagType = getNtagTypeUL(mode);  // NTAGの容量タイプを取得する
      // ページ設定値を更新する
      if (_ntagType != NT_UNKNOWN) {
        _maxPageUL = getMaxPageUL(_ntagType);
        _configPageUL = getConfigPageUL(_ntagType);
        _mounted = true;
        if (_debug) sp("Mifare Ultralight mounted");
      } else {
        stat = false;
      }
    }
    // if (!stat && _debug) sp("mount failed");
  }
  _lastProtectMode = (mode != PRT_AUTO) ? mode : PRT_NOPASS_RW;
  return stat;
}

// カードのマウントを解除する
void NfcEasyWriter::unmountCard() {
  mfrc522.PICC_HaltA();
  _lastProtectMode = PRT_NOPASS_RW;
  _cardType = UnknownCard;
  _ntagType = NT_UNKNOWN;
  _mounted = false;
  delay(50);
  if (_debug) sp("unmounted");
}

// UIDを文字列で返す
String NfcEasyWriter::getUidString() {
  String text = "";
  char buff[3];
  if (_mounted) {
    for (int i=0; i<mfrc522.uid.size; i++) {
      sprintf(buff, "%02X", mfrc522.uid.uidByte[i]);
      if (i > 0) text += ":";
      text += String(buff);
    }
  }
  return text;
}

// カードの種類を大まかに判定する
CardType NfcEasyWriter::checkCardType(MFRC522_I2C &mfrc522) {
  CardType cardType = CardType::UnknownCard;
  byte piccType = mfrc522.PICC_GetType(mfrc522.uid.sak);
  if (piccType == MFRC522_I2C::PICC_TYPE_MIFARE_1K ||
      piccType == MFRC522_I2C::PICC_TYPE_MIFARE_4K) {
    cardType = CardType::Classic;          // Mifare Classic
  } else if (piccType == MFRC522_I2C::PICC_TYPE_MIFARE_UL) {
    cardType = CardType::Ultralight;       // Mifare Ultralight
  }
  return cardType;
}

// カードがマウントされているか？（mountCard()が成功したか見てるだけ）
bool NfcEasyWriter::isMounted() {
  return _mounted;
}

// カードの種類 Mifare Classicか？
bool NfcEasyWriter::isClassic() {
  if (!_mounted) return false;
  return (_cardType == CardType::Classic);
}

// カードの種類 Mifare Ultralightか？
bool NfcEasyWriter::isUltralight() {
  if (!_mounted) return false;
  return (_cardType == CardType::Ultralight);
}

// [Ultralight] NTAGの容量タイプを取得する
NtagType NfcEasyWriter::getNtagTypeUL(ProtectMode mode) {
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  if (!waitCard(5000)) return NT_UNKNOWN;  // 読み書きできる状態になるまで待つ
  NtagType ntag = NT_UNKNOWN;
  byte data[16];

  // 認証がかかっている場合は、まず認証する
  if (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO) {
    if (! authUL(true)) return NT_UNKNOWN;   
  }

  //　NFCから容量情報を読み込む
  if (rawReadUL(data, sizeof(data), 0)) {
    if (_debug) {
      spn("getUltralightSize(): ");
      printDump1Line(data, sizeof(data));
    }
    if (data[3*4] == 0xE1) {
      switch (data[3*4+2]) {  // Page 3 Byte 2 : CC 
        case 0x12: ntag = NT_NTAG213; break;
        case 0x3E: ntag = NT_NTAG215; break;
        case 0x6D: ntag = NT_NTAG216; break;
        default:   ntag = NT_UNKNOWN; break;
      }
    }
  } else {
    if (_debug) sp("getNtagTypeUL() cant rawReadUL");
  }
  return ntag;
}

// [Ultralight] 書き込み可能なページ最大値を取得する
uint8_t NfcEasyWriter::getMaxPageUL(NtagType ntag) {
  switch (ntag) {
    case NT_NTAG213: return 39;
    case NT_NTAG215: return 129;
    case NT_NTAG216: return 225;
    default:         return 0;  // 不明なタイプの場合
  }
}

// [Ultralight] 設定ページのページ位置を取得する
uint8_t NfcEasyWriter::getConfigPageUL(NtagType ntag) {
  switch (ntag) {
    case NT_NTAG213: return 41;
    case NT_NTAG215: return 131;
    case NT_NTAG216: return 227;
    default:         return 0;  // 不明なタイプの場合
  }
}

// 使用可能な容量（仮想アドレス換算）を取得する
uint16_t NfcEasyWriter::getVCapacities() {
  uint16_t size = 0;
  if (isClassic()) {
    size = (_maxSectorCL - _minSectorCL + 1) * 16 * 3;
  } else if (isUltralight()) {
    size = (_maxPageUL - _minPageUL + 1) * 4;
  }
  return size;
}

// [Ultralight] 物理アドレス指定　4ページ(16バイト)読み込む
bool NfcEasyWriter::rawReadUL(byte* data, size_t dataSize, uint8_t page) {
  if (data == nullptr || dataSize != 16) return false;
  byte buff[18];
  byte buffSize = sizeof(buff);
  memset(data, 0, dataSize);
  if (mfrc522.MIFARE_Read(page, buff, &buffSize) == MFRC522_I2C::STATUS_OK) {
    memcpy(data, buff, dataSize);
    return true;
  }
  return false;
}

// [Ultralight] 物理アドレス指定　1ページ(4バイト)書き込む
bool NfcEasyWriter::rawWriteUL(byte* data, size_t dataSize, uint8_t page) {
  if (data == nullptr || dataSize != 4) return false;
  return (mfrc522.MIFARE_Ultralight_Write(page, data, _writeLengthUL) == MFRC522_I2C::STATUS_OK);
}

// 仮想アドレスから物理アドレスに変換する
PhyAddr NfcEasyWriter::addr2PhysicalAddr(uint16_t vaddr, CardType cardtype) {
  PhyAddr pa;
  if (cardtype == CardType::Classic) {
    pa.sector = vaddr / (3 * _writeLengthCL);
    pa.block = (vaddr - pa.sector * (3 * _writeLengthCL)) / _writeLengthCL;
    pa.sector += _minSectorCL;
    pa.blockAddr = pa.sector * 4 + pa.block; 
  } else if (cardtype == CardType::Ultralight) {
    pa.blockAddr = vaddr / _writeLengthUL + _minPageUL;
    if (pa.blockAddr > 255) pa.blockAddr = 255;
  }
  return pa;
}

// [共通] カードからバイト配列型へデータを読み込む
bool NfcEasyWriter::readData(uint16_t vaddr, void* data, size_t dataSize, ProtectMode mode) {
  if (! isMounted()) return false;
  if (mode == PRT_AUTO) mode = _lastProtectMode;

  bool res = false;
  if (_cardType == CardType::Classic) {
    res = readDataCL(vaddr, reinterpret_cast<byte *>(data), dataSize, mode);
  } else if (_cardType == CardType::Ultralight) {
    res = readDataUL(vaddr, reinterpret_cast<byte *>(data), dataSize, mode);
  }
  return res;
}

// [Classic] カードからバイト配列型へデータを読み込む
bool NfcEasyWriter::readDataCL(uint16_t vaddr, byte* data, size_t dataSize, ProtectMode mode) {
  if (! isClassic()) return false;
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  if (vaddr % _writeLengthCL != 0) return false;  // 16バイト単位ではないアドレスは拒否
  if (!waitCard(5000)) return false;  // 読み書きできる状態になるまで待つ

  // ブロックごとのループ
  size_t cplen;
  byte buffer[18];
  byte bufferSize = sizeof(buffer);
  int remain = dataSize;
  size_t index = 0;
  bool abort = false;
  bool protect = (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO);

  while (remain > 0) {
    // 読み込みセクタ/ブロックまたはページを求める
    PhyAddr pa = addr2PhysicalAddr(vaddr + index, CardType::Classic);
    if (_debug) {
      String keyStr = (protect ? "B" : "A");
      spf("Index=%d 読み込み元 Sector/Block=%d/%d -> blockAddr=%d key=%s\n", index, pa.sector, pa.block, pa.blockAddr, keyStr);
    }
    // 認証
    auto usekey = (protect) ? MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_B : MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A;
    auto keyRead = (protect) ? _authKeyB : _authKeyA;
    if (mfrc522.PCD_Authenticate(usekey, pa.blockAddr, &keyRead, &(mfrc522.uid)) != MFRC522_I2C::STATUS_OK) { // 認証
      if (_debug) sp("  認証失敗");
      abort = true;
    }
    // 読み込み実行
    if (!abort && mfrc522.MIFARE_Read(pa.blockAddr, buffer, &bufferSize) == MFRC522_I2C::STATUS_OK) {
        // データをコピー
        cplen = ((remain - _readLength) < 0) ? remain : _readLength;
        memcpy(((byte*)data) + index, buffer, cplen);
        if (_debug) {
          spn("  Data: ");
          printDump1Line(buffer, sizeof(buffer));
        }
    } else {
      if (_debug) sp(".. 読み込み失敗");
      abort = true;
    }
    if (abort) break;
    index += cplen;
    remain -= cplen;
  }//while-remain

  // 認証終了
  mfrc522.PCD_StopCrypto1();
  if (abort) return false;

  return true;
}

// [Ultralight] カードからバイト配列型へデータを読み込む
bool NfcEasyWriter::readDataUL(uint16_t vaddr, byte* data, size_t dataSize, ProtectMode mode) {
  if (! isUltralight()) return false;
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  if (vaddr % _writeLengthUL != 0) return false;  // 4バイト単位ではないアドレスは拒否
  if (!waitCard(5000)) return false;  // 読み書きできる状態になるまで待つ

  // ブロックごとのループ
  size_t cplen;
  byte buffer[18];
  byte bufferSize = sizeof(buffer);
  int remain = dataSize;
  size_t index = 0;
  bool abort = false;

  // 認証がかかっている場合は、まず認証する
  if (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO) {
    if (! authUL(true)) return false;   
  }

  // ブロックごとのループ　最小読み込み単位ごとに分割して読み込む
  while (remain > 0) {
    // 読み込みセクタ/ブロックまたはページを求める
    PhyAddr pa = addr2PhysicalAddr(vaddr + index, CardType::Ultralight);
    if (_debug) {
      spf("Index=%d 読み込み元 Page=%d\n", index, pa.blockAddr);
    }
    // 読み込み実行
    if (!abort && mfrc522.MIFARE_Read(pa.blockAddr, buffer, &bufferSize) == MFRC522_I2C::STATUS_OK) {
        // データをコピー
        cplen = ((remain - _readLength) < 0) ? remain : _readLength;
        memcpy(((byte*)data) + index, buffer, cplen);
        if (_debug) {
          spn("  Data: ");
          printDump1Line(buffer, sizeof(buffer));
        }
    } else {
      if (_debug) sp(".. 読み込み失敗");
      abort = true;
    }
    if (abort) break;
    index += cplen;
    remain -= cplen;
  }//while-remain
  if (abort) return false;

  return true;
}

// [共通] バイト配列型のデータをカードに書き込む
bool NfcEasyWriter::writeData(uint16_t vaddr, void* data, size_t dataSize, ProtectMode mode) {
  if (! isMounted()) return false;
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  if (_debug) Serial.println("Total Data size="+String(dataSize));

  bool res = false;
  if (_cardType == CardType::Classic) {
    res = writeDataCL(vaddr, reinterpret_cast<byte *>(data), dataSize, mode);
  } else if (_cardType == CardType::Ultralight) {
    res = writeDataUL(vaddr, reinterpret_cast<byte *>(data), dataSize, mode);
  }
  return res;
}

// [Classic] バイト配列型のデータをカードに書き込む
bool NfcEasyWriter::writeDataCL(uint16_t vaddr, byte* data, size_t dataSize, ProtectMode mode) {
  if (! isClassic()) return false;
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  if (vaddr % _writeLengthCL != 0) return false;  // 16バイト単位ではないアドレスは拒否
  if (!waitCard(5000)) return false;  // 読み書きできる状態になるまで待つ

  // 準備
  byte buffer[_writeLengthCL];
  int remain = dataSize;
  size_t index = 0;
  bool abort = false;
  bool protect = (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO);

  // ブロックごとのループ　最小書き込み単位ごとに分割して書き込む
  while (remain > 0) {
    memset(buffer, 0, sizeof(buffer));
    size_t cplen = ((remain - _writeLengthCL) < 0) ? remain : _writeLengthCL;
    memcpy(buffer, ((byte*)data) + index, cplen);

    // 書き込みセクタ/ブロックを求める
    PhyAddr pa = addr2PhysicalAddr(vaddr + index, CardType::Classic);
    if (pa.sector > _maxSectorCL) return false;
    if (pa.sector < _minSectorCL) return false;
    if (pa.block >= 3) return false;
    if (_debug) {
      String keyStr = (protect ? "B" : "A");
      spf("Index=%d 書き込み先 Sector/Block=%d/%d -> blockAddr=%d key=%s\n", index, pa.sector, pa.block, pa.blockAddr, keyStr);
      spn("  Data: ");
      printDump1Line(buffer, sizeof(buffer));
    }

    // 認証
    auto usekey = (protect) ? MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_B : MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A;
    auto keyWrite = (protect) ? _authKeyB : _authKeyA;
    if (mfrc522.PCD_Authenticate(usekey, pa.blockAddr, &keyWrite, &(mfrc522.uid)) == MFRC522_I2C::STATUS_OK) {
      // 書き込み
      if (mfrc522.MIFARE_Write(pa.blockAddr, buffer, _writeLengthCL) == MFRC522_I2C::STATUS_OK) {
        if (_debug) sp("  書き込み成功");
      } else {
        if (_debug) sp("  書き込み失敗");
        abort = true;
      }
    } else {
      if (_debug) sp("  認証失敗");
      abort = true;
    }

    if (abort) break;
    index += cplen;
    remain -= cplen;
  }//while-remain

  // 認証終了
  mfrc522.PCD_StopCrypto1();
  if (abort) return false;

  return true;
}

// [Ultralight] バイト配列型のデータをカードに書き込む
bool NfcEasyWriter::writeDataUL(uint16_t vaddr, byte* data, size_t dataSize, ProtectMode mode) {
  if (! isUltralight()) return false;
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  if (vaddr % _writeLengthUL != 0) return false;  // 4バイト単位ではないアドレスは拒否
  if (!waitCard(5000)) return false;  // 読み書きできる状態になるまで待つ

  // 準備
  byte buffer[_writeLengthUL];
  int remain = dataSize;
  size_t index = 0;

  // 認証がかかっている場合は、まず認証する
  if (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO) {
    if (! authUL(true)) return false;   
  }

  // ブロックごとのループ　最小書き込み単位ごとに分割して書き込む
  while (remain > 0) {
    for (int i=0; i<_writeLengthUL; i++) {
      buffer[i] = 0;
    }
    size_t cplen = ((remain - _writeLengthUL) < 0) ? remain : _writeLengthUL;
    memcpy(buffer, ((byte*)data) + index, cplen);

    // 書き込みページを求める
    PhyAddr pa = addr2PhysicalAddr(vaddr + index, CardType::Ultralight);
    if (pa.blockAddr > _maxPageUL) return false;
    if (pa.blockAddr < _minPageUL) return false;
    if (_debug) {
      spf("Index=%d 書き込み先 Page=%d\n", index, pa.blockAddr);
      spn("  Data: ");
      printDump1Line(buffer, sizeof(buffer));
    }

    // 書き込み
    if (mfrc522.MIFARE_Ultralight_Write(pa.blockAddr, buffer, _writeLengthUL)  == MFRC522_I2C::STATUS_OK) {
      if (_debug) sp("..ok");
    } else {
      if (_debug) sp(".. 書き込み失敗");
      return false;
    }
    index += cplen;
    remain -= cplen;
  }//while-remain

  return true;
}

// 認証キーを設定する（書き込みはしない）
void NfcEasyWriter::setAuthKey(AuthKey* key) {
  memcpy(_authKeyB.keyByte, key->keyByte, sizeof(_authKeyB.keyByte));  // 6 bytes for Classic
  if (_debug) {
    spn("新パスワード ");
    printDump1Line(_authKeyB.keyByte, sizeof(_authKeyB.keyByte));
  }
}
void NfcEasyWriter::setAuthKey(MFRC522_I2C::MIFARE_Key* key) {
  setAuthKey(reinterpret_cast<AuthKey *>(key));
}

// プロテクトモード未指定時(PRT_AUTO)のデフォルト動作を指定する（書き込みはしない）
void NfcEasyWriter::setNowProtectMode(ProtectMode mode) {
  _lastProtectMode = mode;
}

// [共通] プロテクトモードや認証キーを書き込む
bool NfcEasyWriter::writeProtect(ProtectMode mode, AuthKey* key, uint16_t vaddr, int size, ProtectMode lastmode) {
  if (! isMounted()) return false;
  if (lastmode == PRT_AUTO) lastmode = _lastProtectMode;

  bool res = false;
  if (_cardType == CardType::Classic) {
    res = writeProtectCL(mode, key, vaddr, size, lastmode);
  } else if (_cardType == CardType::Ultralight) {
    res = writeProtectUL(mode, key, vaddr, false, lastmode);
  }
  return res;
}

// [Classic] プロテクトモードや認証キーを書き込む（指定した仮想アドレスの範囲にあるセクター全て）
bool NfcEasyWriter::writeProtectCL(ProtectMode mode, AuthKey* key, uint16_t vaddr, int size, ProtectMode lastmode) {
  if (! isClassic()) return false;
  if (lastmode == PRT_AUTO) lastmode = _lastProtectMode;
  bool bfProt;//, afProt;
  if (vaddr % 48 != 0) return false;  // セクター単位で行うのでブロックの途中からは受け付けない
  if (!waitCard(5000)) return false;  // 読み書きできる状態になるまで待つ

  // Access Bitの計算　運用方針：KeyAはデフォルト値のまま運用、KeyBはパスワード認証モードのときだけ使用
  uint8_t dataBit, accBit;
  byte accessCondition[3];
  byte buffer[16];
  bfProt = (lastmode == PRT_PASSWD_RW || lastmode == PRT_PASSWD_RO);
  // afProt = (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO);
  if (mode == PRT_NOPASS_RW) {          // KeyAで読み込み可 KeyAで書き込み可
    dataBit = 0b000;
    accBit  = 0b001;  // KeyAで変更
  } else if (mode == PRT_NOPASS_RO) {   // KeyAで読み込み可 書き込み不可
    dataBit = 0b010;
    accBit  = 0b001;  // KeyAで変更
  } else if (mode == PRT_PASSWD_RW) {   // KeyBで読み込み可 KeyBで書き込み可
    dataBit = 0b011;
    accBit  = 0b011;  // KeyBで変更
  } else if (mode == PRT_PASSWD_RO) {   // KeyBで読み込み可 書き込み不可
    dataBit = 0b101;
    accBit  = 0b011;  // KeyBで変更
  } else {
    return false;
  }
  mfrc522.MIFARE_SetAccessBits(accessCondition, dataBit, dataBit, dataBit, accBit);
  memcpy(buffer, _authKeyA.keyByte, 6);
  memcpy(buffer + 6, accessCondition, 3);  // Access Bit
  buffer[9] = (uint8_t) mode;   // User Data
  // memcpy(buffer + 10, ((key != nullptr) ? key : _authKeyBDefault.keyByte), 6);
  if (key != nullptr) memcpy(buffer + 10, key, 6);
  else memcpy(buffer + 10, _authKeyBDefault.keyByte, 6);
  if (_debug) {
    spn("Writing Block3 Data: ");
    printDump1Line(buffer, sizeof(buffer));
  }

  // セクタートレーラー(Block3)にデータを書き込む
  int remain = size;
  size_t index = 0;
  bool abort = false;
  // セクタごとのループ
  while (remain > 0) {
    PhyAddr pa = addr2PhysicalAddr(vaddr + index, CardType::Classic);
    uint16_t blockAddr = pa.sector * 4 + 3;
    if (_debug) {
      String keyStr = (bfProt ? "B" : "A");
      spf("Index=%d 書き込み先 Sector/Block=%d/3 -> blockAddr=%d key=%s ", index, pa.sector, blockAddr, keyStr);
    }

    // 認証開始
    auto usekey = (bfProt) ? MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_B : MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A;
    auto keyWrite = (bfProt) ? _authKeyB : _authKeyA;
    if (mfrc522.PCD_Authenticate(usekey, blockAddr, &keyWrite, &(mfrc522.uid)) == MFRC522_I2C::STATUS_OK) {
      // 書き込み
      if (mfrc522.MIFARE_Write(blockAddr, buffer, _writeLengthCL) == MFRC522_I2C::STATUS_OK) {
        if (_debug) sp("  書き込み成功");
      } else {
        if (_debug) sp("  書き込み失敗");
        abort = true;
      }
    } else {
      if (_debug) sp("  認証失敗");
      abort = true;
    }
    if (abort) break;
    index += 48;
    remain -= 48;
  }
  // 認証終了
  mfrc522.PCD_StopCrypto1();
  if (!abort) _lastProtectMode = mode;
  return !abort;
}

// [Ultralight] プロテクトモードや認証キーを書き込む（指定した仮想アドレス以降のにあるページ全て）
bool NfcEasyWriter::writeProtectUL(ProtectMode mode, AuthKey* key, uint16_t vaddr, bool phyaddr, ProtectMode lastmode) {
  if (! isUltralight()) return false;
  if (lastmode == PRT_AUTO) lastmode = _lastProtectMode;
  bool bfProt, afProt, aReado;
  if (vaddr % 4 != 0 && !phyaddr) return false;  // ページ単位で行うのでページの途中からは受け付けない
  if (!waitCard(5000)) return false;  // 読み書きできる状態になるまで待つ
  if (mode == PRT_NOPASS_RO) return false;  // UltralightにはPWなしReadonlyは無いのでエラーで返す

  // 準備
  PhyAddr pa;
  if (phyaddr) pa.blockAddr = vaddr;  // 物理アドレス指定 
  else pa = addr2PhysicalAddr(vaddr, CardType::Ultralight);   // 仮想アドレス指定
  bfProt = (lastmode == PRT_PASSWD_RW || lastmode == PRT_PASSWD_RO);
  afProt = (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO);
  aReado = (mode == PRT_NOPASS_RO || mode == PRT_PASSWD_RO);

  // 現在の設定情報を読み込む
  ULConfig ulconf;
  if (! readConfigDataUL(&ulconf, lastmode)) return false;

  // 新しい設定情報を書き込む
  ulconf.ACCESS = (ulconf.ACCESS & 0x7F) | (!aReado << 7);  // PROT in ACCESS in CFG2[0]
  ulconf.AUTH0 = (afProt) ? pa.blockAddr : 0xFF; // AUTH0 in CFG1[3]
  if (key != nullptr) {
    memcpy(ulconf.PWD4, key->keyByte, sizeof(ulconf.PWD4));
    memcpy(ulconf.PACK, ((byte*)key->keyByte)+4, sizeof(ulconf.PACK));
  } else {
    memcpy(ulconf.PWD4, _authKeyBDefault.keyByte, sizeof(ulconf.PWD4));
    memcpy(ulconf.PACK, ((byte*)_authKeyBDefault.keyByte)+4, sizeof(ulconf.PACK));
  }
  bool res = writeConfigDataUL(&ulconf, lastmode);
  if (res) _lastProtectMode = mode;
  return res;
}

// [Ultralight] パスワード認証を行う
bool NfcEasyWriter::authUL(bool checkPack) {
  byte password[4];
  byte pack[4];
  memcpy(password, _authKeyB.keyByte, sizeof(password));
  byte passwordLen = sizeof(password);
  byte packLen = sizeof(pack);
  byte result = mfrc522.MIFARE_Ultralight_Authenticate(password, &passwordLen, pack, &packLen);
  if (_debug) {
    spf("認証結果 authUL() result=%d, send password=",result);
    printDump1Line(password, passwordLen);
  }
  if (result != MFRC522_I2C::STATUS_OK) return false;
  if (checkPack) {
    if (_debug) spf("received pack=%02X %02X\n", pack[0], pack[1]);
    return (pack[0] == _authKeyB.keyByte[4] && pack[1] == _authKeyB.keyByte[5]);
  }
  return true;
}

// [Ultralight] パスワード認証を無効化にする（リセット）
bool NfcEasyWriter::unauthUL(ProtectMode mode) {
  unmountCard();  // パスワードを無効にするためにHALTを実行する
  return mountCard(5000, mode); // 再マウント（Select実行）
}

// [Ultralight] 設定情報を取得する
bool NfcEasyWriter::readConfigDataUL(ULConfig* ulconf, ProtectMode mode) {
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  if (_configPageUL == 0) return false;
  if (!waitCard(5000)) return false;  // 読み書きできる状態になるまで待つ
  memset(ulconf, 0, sizeof(ULConfig));

  // 認証がかかっている場合は、まず認証する
  if (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO) {
    if (! authUL(true)) return false;   
  }

  // データ取得
  byte data[16];
  if (_debug) spf("設定情報を取得 page=%d\n", _configPageUL);
  if (rawReadUL(data, sizeof(data), _configPageUL)) {
    if (_debug) {
      spn("RAW Data: ");
      printDump1Line(data, sizeof(data));
      // printDumpBin(data, sizeof(data));
    }
    memcpy(ulconf, data, sizeof(ULConfig));
    return true;
  } else {
    if (_debug) sp("  読み込み失敗");
  }
  return false;
}
bool NfcEasyWriter::readConfigDataUL(ULConfigEx* ulconfex, ProtectMode mode) {
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  memset(ulconfex, 0, sizeof(ULConfigEx));
  if (readConfigDataUL(&ulconfex->ulconf, mode)) {
    ulconfex->PROT = (ulconfex->ulconf.ACCESS & 0x80);
    ulconfex->CFGLCK = (ulconfex->ulconf.ACCESS & 0x40);
    ulconfex->AUTHLIM = ulconfex->ulconf.ACCESS & 0x07;
    return true;
  }
  return false;
}

// [Ultralight] の設定情報を書き込む
bool NfcEasyWriter::writeConfigDataUL(ULConfig* ulconf, ProtectMode mode) {
  if (mode == PRT_AUTO) mode = _lastProtectMode;
  if (_configPageUL <= 3) return false;
  if (!waitCard(5000)) return false;  // 読み書きできる状態になるまで待つ
  byte data[4];

  // 認証がかかっている場合は、まず認証する
  if (mode == PRT_PASSWD_RW || mode == PRT_PASSWD_RO) {
    if (! authUL(true)) return false;   
  }

  // 設定情報を書き込む
  if (_debug) sp("設定情報を書き込む");
  for (uint8_t idx=0; idx<16; idx+=4) {
    uint8_t page = _configPageUL+(idx/4);
    memcpy(data, ((byte*)ulconf) + idx, 4);
    if (_debug) {
      spn("  ");
      printDump(data, sizeof(data), " ", "", "");
    }
    if (rawWriteUL(data, sizeof(data), page)) {
      if (_debug) spf(" page=%d 書き込み成功\n", page);
    } else {
      if (_debug) spf(" page=%d 書き込み失敗\n", page);
      return false;
    }
  }
  return true;
}

// データ領域のゼロクリア（NDEFメッセージを削除し、無効なTLVデータを書き込む）
bool NfcEasyWriter::format(bool formatAll) {
  if (! isMounted()) return false;
  byte buff[8] = { 0xFE, 0, 0, 0, 0, 0, 0, 0 };   // Terminator TLV

  // [Ultralight] page.4のNDEFメッセージを削除する（データはpage.5から始まる）
  if (_cardType == CardType::Ultralight) {
    if (!mfrc522.MIFARE_Ultralight_Write(4, buff, 4) == MFRC522_I2C::STATUS_OK) return false;
    // if (!mfrc522.MIFARE_Ultralight_Write(5, &buff[4], 4) == MFRC522_I2C::STATUS_OK) return false;
  }

  // 全領域を0で埋める
  if (formatAll) {
    uint16_t totalSize = getVCapacities();  // 使用可能な容量（仮想アドレス換算）を取得する
    byte wdata[totalSize] = {0};
    return writeData(0, wdata, sizeof(wdata));  // 書き込み
  }

  return true;
}

// ファームウェアバージョンのチェック
bool NfcEasyWriter::firmwareVersionCheck() {
	byte ver = mfrc522.PCD_ReadRegister(MFRC522_I2C::VersionReg);
  if (_debug) spp("firmware version", ver);
  // return (ver == 0x88 || ver == 0x90 || ver == 0x91 || ver == 0x92);
  return (ver > 0 && ver < 255);  // M5Stack RFID 2 Unitは0x15を返すので、何らかの値が返ってくればヨシ!とする
}

//
// 以下はデバッグ用 -----------------------------------------------------------------
//

// [Classic] 壊してしまったセクタートレーラーを修復する
bool NfcEasyWriter::recoverySectorTruckCL(uint16_t blockAddr, AuthKey* key, bool useKeyB) {
  if (! isClassic()) return false;
  if (blockAddr < 7) return false;
  if (blockAddr % 4 != 3) return false;
  bool abort = false;

  // デフォルト時データ作成
  byte buffer[16];
  memset(buffer, 0xFF, sizeof(buffer));
  buffer[6] = 0xFF;
  buffer[7] = 0x07;
  buffer[8] = 0x80;
  buffer[9] = 0x69;

  // 認証開始
  MFRC522_I2C::MIFARE_Key mifarekey;
  memcpy(&mifarekey, key, sizeof(MFRC522_I2C::MIFARE_Key));
  spf("セクタートレーラー修復 blockAddr=%d key=%s\n", blockAddr, (useKeyB?"B":"A") );
  auto usekey = (useKeyB) ? MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_B : MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A;
  if (mfrc522.PCD_Authenticate(usekey, blockAddr, &mifarekey, &(mfrc522.uid)) == MFRC522_I2C::STATUS_OK) {
    // 書き込み
    if (mfrc522.MIFARE_Write(blockAddr, buffer, _writeLengthCL) == MFRC522_I2C::STATUS_OK) {
      if (_debug) sp("  書き込み成功");
    } else {
      if (_debug) sp("  書き込み失敗");
      abort = true;
    }
  } else {
    if (_debug) sp("  認証失敗");
    abort = true;
  }

  // 認証終了
  mfrc522.PCD_StopCrypto1();
  return !abort;
}

// [Ultralight] 壊してしまった設定情報を修復する
bool NfcEasyWriter::recoveryConfigDataUL(bool useAuth, AuthKey* key, ProtectMode lastmode) {
  if (! isUltralight()) return false;
  if (lastmode == PRT_AUTO) lastmode = _lastProtectMode;
  bool res;

  // デフォルト時データ作成
  byte data[16];
  ULConfig ulconf;
  memset(data, 0, sizeof(data));
  data[0] = 0x04; // MIRROR
  data[3] = 0xFF; // AUTH0
  data[5] = 0x05; // RFUI1
  memcpy(&ulconf, data, sizeof(ULConfig));
  if (_debug) {
    spf("設定情報修復 page=%d data=", _configPageUL);
    printDump1Line(data, sizeof(data));
    sp("");
  }

  // パスワードの仮設定
  MFRC522_I2C::MIFARE_Key backup;
  if (useAuth) {
    backup = _authKeyB;
    setAuthKey(key);
  } 

  // 書き込み
  res = writeConfigDataUL(&ulconf, lastmode);
  if (_debug) spf("  書き込み=%s\n", (res) ? "成功" : "失敗");
  if (useAuth) _authKeyB = backup;

  return res;
}

// 全データをシリアルに出力する　デバッグ用　（MFRC522_I2Cライブラリ標準のdump結果）
void NfcEasyWriter::dumpAllBasic() {
  if (! isMounted()) return;
  if (_cardType == CardType::Classic) {
    if (!waitCard(5000)) return;  // 読み書きできる状態になるまで待つ
    mfrc522.PICC_DumpToSerial(&(mfrc522.uid));
  } else if (_cardType == CardType::Ultralight) {
    mfrc522.PICC_DumpMifareUltralightToSerial();  // この関数は16ページまでしか読まないので全部は見れない
  }
}

// 全データをシリアルに出力する　デバッグ用
void NfcEasyWriter::dumpAll(bool inProtect, uint8_t phySta, uint8_t phyEnd) {
  if (! isMounted()) return;
  byte buffer[18];
  byte bufferSize = sizeof(buffer);
  if (!waitCard(5000)) return;  // 読み書きできる状態になるまで待つ
  bool debugOrig = _debug;
  _debug = false;

  // カード情報
  spn("Card UID: ");
  for (int i=0; i<mfrc522.uid.size; i++) {
    spf("%02X ", mfrc522.uid.uidByte[i]);
  }
  sp("\nCard Type: "+String(mfrc522.PICC_GetTypeName(mfrc522.PICC_GetType(mfrc522.uid.sak))));

  // Mifare Classicの場合
  if (isClassic()) {
    char strs[17] = "\0";
    sp("Page/Blk|BlkAdr|  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 | 0123456789abcdef");
    for (int sector=0; sector <= _maxSectorCL; sector++) {
      for (int block=0; block<4; block++) {
        if (block == 0) sp("---------+------+-------------------------------------------------+");
        uint16_t blockAddr = sector * 4 + block;
        bool protect = (inProtect && phySta <= blockAddr && blockAddr <= phyEnd && block < 3);   // プロテクト範囲はKeyBで認証する
        auto usekey = (protect) ? MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_B : MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A;
        auto keyRead = (protect) ? _authKeyB : _authKeyA;
        if (_dbgopt & NFCOPT_DUMP_NDEF_CLASSIC) {
          usekey = MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A;
          keyRead = (sector == 0) ? _authKeyNdefClassic0 : _authKeyNdefClassic1;
        }
        if (mfrc522.PCD_Authenticate(usekey, blockAddr, &keyRead, &(mfrc522.uid)) == MFRC522_I2C::STATUS_OK) {
          if (mfrc522.MIFARE_Read(blockAddr, buffer, &bufferSize) == MFRC522_I2C::STATUS_OK) {
            String pstr = (protect && block < 3) ? "*" : " ";
            spf("%s%3d / %d |  %3d | ", pstr, sector, block, blockAddr);
            for (int i=0; i < bufferSize-2; i++) {
              spf("%02X ", buffer[i]);
              strs[i] = (buffer[i] >= 0x20 && buffer[i] <= 0x7F) ? buffer[i] : ' ';
            }
            strs[16] = '\0';
            spn("| "+String(strs)+"\n");
          }
        } else {
          spf("auth error %d/%d:%d\n", sector, block, blockAddr);
          if (_dbgopt & NFCOPT_DUMP_AUTHFAIL_CONTINUE) {
            unmountCard();
            mountCard(5000);
            continue;
          }
          break;
        }
      }
      //if (abort) break;
    }
    mfrc522.PCD_StopCrypto1();

  // Mifare Ultralightの場合
  } else if (isUltralight()) {
    bool authed = false;
    char strs[5] = "\0";
    sp("Page : 0  1  2  3  : Text");
    uint8_t maxpage = (_dbgopt & NFCOPT_DUMP_UL255PAGE_READ) ? 255 : _maxPageUL+5;
    for (uint8_t page=0; page<=(maxpage-2); page+=4) {
      if (inProtect && !authed) {
        authUL(false);  // プロテクト時は認証する
        authed = true;
      }
      if (mfrc522.MIFARE_Read(page, buffer, &bufferSize) == MFRC522_I2C::STATUS_OK) {
        for (int i=0; i < bufferSize-2; i++) {
          if (i%4 == 0) {
            String pstr = (inProtect && phySta <= (page+i/4)) ? "*" : " ";
            spf("%s%3d : ", pstr, page+i/4);
          }
          spf("%02X ", buffer[i]);
          strs[i%4] = (buffer[i] >= 0x20 && buffer[i] <= 0x7F) ? buffer[i] : ' ';
          strs[4] = '\0';
          if (i%4 == 3) {
            spn(": "+String(strs)+"\n");
          }
        }
      } else {
        spf("auth error %d\n", page);
        // if (_dbgopt & NFCOPT_DUMP_AUTHFAIL_CONTINUE) { // 無意味なので廃止
        //   unmountCard();
        //   mountCard(5000);
        //   authed = false;
        //   continue;
        // }
        break;
      }
    }
  } else {
    sp("UnknownCard Card Type");
  }

  _debug = debugOrig;
}

// バイナリのdumpを出力する 16進数表示
void NfcEasyWriter::printDump(const byte *data, size_t dataSize, String sepa, String cr, String crend) {
  if (data == nullptr) return;
  size_t i;
  for (i=0; i<dataSize; i++) {
    Serial.printf("%02X%s", data[i], sepa);
    if (i % 16 == 15) Serial.print(cr);
  }
  if (i % 16 != 15) Serial.print(cr);
  Serial.print(crend);
}
void NfcEasyWriter::printDump1Line(const byte *data, size_t dataSize) {
  printDump(data, dataSize, " ", "", "\n");
}

// 8桁の2進数を返す
String NfcEasyWriter::dec2bin8(uint8_t num) {
  String bin = String(num, BIN);
  String zero = "";
  for (int i=0; i<8-bin.length(); i++) zero += "0";
  return zero + bin;
}

// バイナリのdumpを出力する 2進数表示
void NfcEasyWriter::printDumpBin(const byte *data, size_t dataSize) {
  if (data == nullptr) return;
  for (size_t i=0; i<dataSize; i++) {
    if (i % 4 == 0 && i != 0) sp("");
    spn(dec2bin8(data[i])+" ");
  }
  sp("");
}
