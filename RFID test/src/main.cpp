#include <SPI.h>
#include <MFRC522.h>

// ====== 根据你的接线 ======
#define SS_1 D0   // RFID1 SDA
#define RST_1 D5  // RFID1 RST

#define SS_2 D1   // RFID2 SDA
#define RST_2 D6  // RFID2 RST

// 两个 RFID 实例
MFRC522 rfid1(SS_1, RST_1);
MFRC522 rfid2(SS_2, RST_2);

void printUID(MFRC522 &reader) {
  for (byte i = 0; i < reader.uid.size; i++) {
    if (reader.uid.uidByte[i] < 0x10) {
      Serial.print("0");
    }
    Serial.print(reader.uid.uidByte[i], HEX);
    if (i < reader.uid.size - 1) {
      Serial.print(" ");
    }
  }
  Serial.println();
}

void checkReader(MFRC522 &reader, const char *name) {
  // 检查是否有新卡
  if (!reader.PICC_IsNewCardPresent()) {
    return;
  }

  // 读取卡序列号
  if (!reader.PICC_ReadCardSerial()) {
    return;
  }

  Serial.print(name);
  Serial.print(" detected card. UID: ");
  printUID(reader);

  // 停止读卡
  reader.PICC_HaltA();
  reader.PCD_StopCrypto1();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Dual RFID RC522 Test");

  // 如果 SPI 线已经接到开发板默认 SPI 引脚，用这个就行
  SPI.begin();

  // 初始化两个 reader
  rfid1.PCD_Init();
  delay(50);
  rfid2.PCD_Init();
  delay(50);

  Serial.println("RFID1 initialized");
  Serial.println("RFID2 initialized");
  Serial.println("Place a card near reader 1 or reader 2...");
}

void loop() {
  checkReader(rfid1, "RFID1");
  delay(100);

  checkReader(rfid2, "RFID2");
  delay(100);
}