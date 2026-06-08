// ============================================================
// player_card/src/main.cpp
// Seeed XIAO ESP32S3 — Monopoly Player Card (2-player game)
// Hardware: SSD1351 128×128 OLED + PN532 NFC + 5 buttons
//
// Change PLAYER_NUM in platformio.ini build_flags:
//   -DPLAYER_NUM=1   (first player device)
//   -DPLAYER_NUM=2   (second player device)
//
// Buttons:
//   OK    (D9) — confirm / roll dice / buy
//   LEFT  (D6) — back / skip / cancel
//   RIGHT (D7) — alternate action
//   UP    (D0) — scroll up / view properties
//   DOWN  (D4) — scroll down / view cards
//
// BLE messages IN (from central, notify):
//   WAIT | START | TURN:{n} | DICE:{sum}
//   STATE:{p1pos}:{p1bal}:{p2pos}:{p2bal}
//   ACT:GO:{amt} | ACT:BUY:{name}:{price}
//   ACT:RENT:{amt}:{who} | ACT:TAX:{amt}
//   ACT:OWN | ACT:JAIL | ACT:VISIT | ACT:FREE
//   ACT:CHANCE:{text} | ACT:CHEST:{text} | ACT:BAIL:{amt}
//   AUCTION:{sq}:{startPrice} | BID:{playerNum}:{amt}
//   AUCTION:WIN:{n}:{sq}:{price} | AUCTION:NOBID
//   DONE | BANKRUPT:{n} | WIN:{n}
//
// BLE messages OUT (write to central):
//   P{n}:HELLO | P{n}:ROLL | P{n}:BUY | P{n}:SKIP | P{n}:OK
//   P{n}:BID:{amt} | P{n}:PASS
// ============================================================

#include <Arduino.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <Adafruit_PN532.h>
#include <NimBLEDevice.h>

// ── Player identity ──────────────────────────────────────────
#ifndef PLAYER_NUM
#define PLAYER_NUM 1
#endif
#define MY_IDX (PLAYER_NUM - 1)   // 0-based index

// ── OLED SPI pins (XIAO ESP32S3) ────────────────────────────
#define OLED_CS   D1
#define OLED_DC   D2
#define OLED_RST  D3
// SPI: SCK=D8(GPIO7), MOSI=D10(GPIO9)

// ── Button pins (active LOW, INPUT_PULLUP) ───────────────────
#define BTN_OK    D9
#define BTN_LEFT  D6
#define BTN_RIGHT D7
#define BTN_UP    D0

// ── OLED colors (RGB565) ─────────────────────────────────────
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_BLUE    0x339F  // 0x3366FF → RGB565
#define C_GREEN   0x07E0
#define C_YELLOW  0xFFE0
#define C_ORANGE  0xFD20
#define C_CYAN    0x07FF
#define C_MAGENTA 0xF81F
#define C_GRAY    0x7BEF
#define C_LGRAY   0xC618
#define C_DKGRAY  0x39E7
#define C_GOLD    0xFEA0
#define C_PURPLE  0x781F
#define C_PINK    0xFC18
#define C_BROWN   0x8200
#define C_LBLUE   0x5D9F
#define C_PROP_LGREEN 0x65C8  // visible green (~0x60,0xB8,0x40)
#define C_PROP_LYELLOW 0xE650  // warm tan (visible, not white, not pure yellow)
#define C_PROP_DGREY 0x4A49
#define C_PROP_ORANGE 0xFC40
#define C_PROP_SIENNA 0xA285
#define C_PROP_LGBROWN 0xC48E  // light brown (~0xC0,0x90,0x70)

static const uint16_t P_COLOR[2] = { C_RED, C_BLUE };

// ── Board square names (must match central_bank exactly) ─────
#define NUM_SQ 40
static const char* SQ_NAMES[NUM_SQ] = {
  "GO", "Old Kent Road", "Community Chest", "Whitechapel Road",
  "Income Tax", "King's Cross Station", "The Angel Islington", "Chance",
  "Euston Road", "Pentonville Road", "Jail / Just Visiting", "Pall Mall",
  "Electric Company", "Whitehall", "Northumberland Avenue", "Marylebone Station",
  "Bow Street", "Community Chest", "Marlborough Street", "Vine Street",
  "Free Parking", "Strand", "Chance", "Fleet Street",
  "Trafalgar Square", "Fenchurch St. Station", "Leicester Square", "Coventry Street",
  "Water Works", "Piccadilly", "Go To Jail", "Regent Street",
  "Oxford Street", "Community Chest", "Bond Street", "Liverpool St. Station",
  "Chance", "Park Lane", "Super Tax", "Mayfair",
};

static const uint16_t SQ_GRP_COLORS[NUM_SQ] = {
  C_DKGRAY, C_PROP_LGREEN, C_DKGRAY, C_PROP_LGREEN, C_DKGRAY, C_DKGRAY, C_PROP_LYELLOW, C_DKGRAY,
  C_PROP_LYELLOW, C_PROP_LYELLOW, C_DKGRAY, C_PROP_DGREY, C_DKGRAY, C_PROP_DGREY, C_PROP_DGREY, C_DKGRAY,
  C_PROP_SIENNA, C_DKGRAY, C_PROP_SIENNA, C_PROP_SIENNA, C_DKGRAY, C_PROP_ORANGE, C_DKGRAY, C_PROP_ORANGE,
  C_PROP_ORANGE, C_DKGRAY, C_YELLOW, C_YELLOW, C_DKGRAY, C_YELLOW, C_DKGRAY, C_PROP_LGBROWN,
  C_PROP_LGBROWN, C_DKGRAY, C_PROP_LGBROWN, C_DKGRAY, C_DKGRAY, C_BLUE, C_DKGRAY, C_BLUE,
};

static const char* SQ_GRP_NAMES[NUM_SQ] = {
  "", "Brown", "", "Brown", "", "", "Light Blue", "",
  "Light Blue", "Light Blue", "", "Pink", "", "Pink", "Pink", "",
  "Orange", "", "Orange", "Orange", "", "Red", "", "Red",
  "Red", "", "Yellow", "Yellow", "", "Yellow", "", "Green",
  "Green", "", "Green", "", "", "Dark Blue", "", "Dark Blue",
};

// ── NFC card → square index mapping ─────────────────────────
// Assign NFC UIDs to board squares. Update UIDs after reading
// actual tags with the NFC scanner.
struct NfcMap { uint8_t uid[4]; int sq; };
static const NfcMap NFC_CARDS[] = {
  { {0xA1,0xB2,0xC3,0xD4},  0 },  // GO
  { {0x11,0x22,0x33,0x44},  1 },  // Old Kent Rd
  { {0xAA,0xBB,0xCC,0xDD},  5 },  // Kings Cross
  { {0x01,0x02,0x03,0x04},  7 },  // Chance
  { {0xDE,0xAD,0xBE,0xEF}, 10 },  // Jail
  { {0xCA,0xFE,0x00,0x01},  6 },  // Angel Islington
  { {0x12,0x34,0x56,0x78}, 11 },  // Pall Mall
  { {0xFF,0xEE,0xDD,0xCC}, 16 },  // Bow Street
};
#define NFC_MAP_N (sizeof(NFC_CARDS)/sizeof(NFC_CARDS[0]))

// Replace P1_PAY_TAG_UID with Player 1's real payment tag UID.
static const uint8_t P1_PAY_TAG_UID[7] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00};
static const uint8_t P2_PAY_TAG_UID[7] = {0x04,0x19,0x53,0x75,0xCC,0x2A,0x81};

// ── Hardware objects ─────────────────────────────────────────
Adafruit_SSD1351 display(128, 128, &SPI, OLED_CS, OLED_DC, OLED_RST);
Adafruit_PN532*  nfc = nullptr;  // created in setup after Serial/OLED are alive

// ── Game State (local copy maintained from BLE messages) ─────
struct LocalState {
  // My info
  int  myBal;
  int  myPos;
  bool myTurn;
  bool inJail;

  // Other player info
  int  otherBal;
  int  otherPos;

  // Dice
  int  lastDice;       // sum of last roll (0 = not rolled yet)

  // Pending action
  enum ActionT {
    ACT_NONE, ACT_BUY, ACT_RENT, ACT_TAX,
    ACT_INFO,   // generic notification (GO, chance, chest, etc.)
    ACT_WIN, ACT_LOSE
  } action;
  char actionTitle[24];
  char actionDesc[48];
  int  actionAmt;
  int  proxyPayFor;    // other player number this reader can confirm by NFC tag

  // Auction
  bool auctionActive;
  int  auctionSq;         // square index being auctioned
  int  auctionStartPrice; // starting price (list price)
  int  auctionBid;        // current highest bid
  int  auctionBidder;     // player number of highest bidder (1-based, 0=none)
  int  myBidAmt;          // bid amount player is composing (UP/DOWN to adjust)
  uint32_t auctionStartMs; // millis() when auction started (for countdown)

  // Connection
  bool bleConnected;
  bool gameStarted;
  bool gameOver;
} S;

// ── Screen enum ──────────────────────────────────────────────
enum Screen {
  SCR_BOOT,       // boot / BLE scanning
  SCR_LOBBY,      // connected, waiting for game
  SCR_HOME,       // main status (my balance, position, etc.)
  SCR_NFC,        // scan NFC to confirm position
  SCR_ACTION,     // current action (buy / rent / info)
  SCR_WAITING,    // not my turn — show other player's event
  SCR_AUCTION,    // auction in progress — bid / pass
  SCR_PROPERTIES,
  SCR_GAME_OVER
};
Screen curScreen = SCR_BOOT;
bool   needRedraw = true;
static uint32_t lastNfcPayMs = 0;
static Screen returnScreen = SCR_HOME;
static int propScrollOffset = 0;
static int sqOwnerLocal[NUM_SQ];

// ── Button debounce ──────────────────────────────────────────
#define DEBOUNCE_MS  50
#define LONGPRESS_MS 800
struct Btn {
  uint8_t  pin;
  bool     lastRaw;
  bool     pressActive;
  bool     longFired;
  uint32_t lastChangeMs;
  uint32_t pressStartMs;
};
Btn BTNS[4];
enum BtnEvt { EVT_NONE, EVT_SHORT, EVT_LONG };

void initButtons() {
  const uint8_t pins[4] = {BTN_OK, BTN_LEFT, BTN_RIGHT, BTN_UP};
  for (int i = 0; i < 4; i++) {
    pinMode(pins[i], INPUT_PULLUP);
    BTNS[i] = {pins[i], HIGH, false, false, 0, 0};
  }
}

BtnEvt pollButton(Btn& b) {
  bool raw = digitalRead(b.pin);
  uint32_t now = millis();
  if (raw != b.lastRaw) { b.lastRaw = raw; b.lastChangeMs = now; }
  if ((now - b.lastChangeMs) < DEBOUNCE_MS) return EVT_NONE;

  bool stable = raw;
  if (stable == LOW && !b.pressActive) {
    b.pressActive  = true;
    b.pressStartMs = now;
    b.longFired    = false;
  } else if (stable == HIGH && b.pressActive) {
    b.pressActive = false;
    if (!b.longFired) return EVT_SHORT;
  } else if (stable == LOW && b.pressActive && !b.longFired) {
    if ((now - b.pressStartMs) >= LONGPRESS_MS) {
      b.longFired = true;
      return EVT_LONG;
    }
  }
  return EVT_NONE;
}

// ── BLE Client ───────────────────────────────────────────────
#define BLE_SERVER_NAME "MonopolyCentral"
#define BLE_SVC_UUID    "0000AA01-0000-1000-8000-00805F9B34FB"
#define BLE_NOTIFY      "0000BB01-0000-1000-8000-00805F9B34FB"
#define BLE_WRITE       "0000BB02-0000-1000-8000-00805F9B34FB"

static NimBLEAdvertisedDevice*     pServer  = nullptr;
static NimBLEClient*               pClient  = nullptr;
static NimBLERemoteCharacteristic* pWriteRC = nullptr;

// 4-slot queue — prevents message loss when central sends back-to-back notifies
#define BLE_Q_SIZE 8
#define BLE_Q_LEN  96
static char             bleQueue[BLE_Q_SIZE][BLE_Q_LEN];
static volatile uint8_t qHead = 0, qTail = 0;

void bleWrite(const char* msg) {
  if (pWriteRC && S.bleConnected) {
    pWriteRC->writeValue(msg, false);
    Serial.printf("[TX] %s\n", msg);
  }
}

// Format and send player message, e.g. sendCmd("ROLL") → "P1:ROLL"
void sendCmd(const char* cmd) {
  char buf[24];
  snprintf(buf, sizeof(buf), "P%d:%s", PLAYER_NUM, cmd);
  bleWrite(buf);
}

void sendPlayerCmd(int playerNum, const char* cmd) {
  char buf[24];
  snprintf(buf, sizeof(buf), "P%d:%s", playerNum, cmd);
  bleWrite(buf);
}

void onBLENotify(NimBLERemoteCharacteristic*, uint8_t* data,
                 size_t len, bool) {
  uint8_t next = (qTail + 1) % BLE_Q_SIZE;
  if (next != qHead) {  // drop only when queue is full (shouldn't happen)
    snprintf(bleQueue[qTail], BLE_Q_LEN, "%.*s",
             (int)min(len, (size_t)(BLE_Q_LEN - 1)), (char*)data);
    qTail = next;
  }
}

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*) override {
    S.bleConnected = false;
    pClient  = nullptr;
    pWriteRC = nullptr;
    needRedraw = true;
    NimBLEDevice::getScan()->start(10, nullptr, false);
    Serial.println("[BLE] Disconnected — scanning");
  }
};

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* d) override {
    if (d->getName() == BLE_SERVER_NAME) {
      pServer = d;
      NimBLEDevice::getScan()->stop();
      Serial.println("[BLE] Found MonopolyCentral");
    }
  }
};

bool bleDoConnect() {
  Serial.println("[BLE] Connecting...");
  NimBLEClient* c = NimBLEDevice::createClient();
  c->setClientCallbacks(new ClientCB(), false);
  if (!c->connect(pServer)) {
    NimBLEDevice::deleteClient(c);
    return false;
  }
  NimBLERemoteService* svc = c->getService(BLE_SVC_UUID);
  if (!svc) { c->disconnect(); NimBLEDevice::deleteClient(c); return false; }
  auto* nrChr = svc->getCharacteristic(BLE_NOTIFY);
  if (nrChr && nrChr->canNotify()) nrChr->subscribe(true, onBLENotify);
  pWriteRC    = svc->getCharacteristic(BLE_WRITE);
  pClient     = c;
  pServer     = nullptr;
  S.bleConnected = true;
  needRedraw  = true;
  Serial.println("[BLE] Connected!");
  sendCmd("HELLO");
  return true;
}

void setupBLE() {
  NimBLEDevice::init("MonopolyPlayer");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCB(), false);
  scan->setActiveScan(true);
  scan->setInterval(100);
  scan->setWindow(99);
  scan->start(10, nullptr, false);
}

// ── NFC Scanner ──────────────────────────────────────────────
static bool nfcAvail = false;

int lookupNfcUID(uint8_t* uid, uint8_t len) {
  if (len != 4) return -1;
  for (size_t i = 0; i < NFC_MAP_N; i++) {
    bool match = true;
    for (int b = 0; b < 4; b++) {
      if (NFC_CARDS[i].uid[b] != uid[b]) { match = false; break; }
    }
    if (match) return NFC_CARDS[i].sq;
  }
  return -1;  // unknown tag
}

bool isP2PayTag(uint8_t* uid, uint8_t len) {
  if (len != sizeof(P2_PAY_TAG_UID)) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] != P2_PAY_TAG_UID[i]) return false;
  }
  return true;
}

bool isP1PayTag(uint8_t* uid, uint8_t len) {
  if (len != sizeof(P1_PAY_TAG_UID)) return false;
  for (uint8_t i = 0; i < len; i++) {
    if (uid[i] != P1_PAY_TAG_UID[i]) return false;
  }
  return true;
}

bool payTagMatches(int playerNum, uint8_t* uid, uint8_t len) {
  if (playerNum == 1) return isP1PayTag(uid, len);
  if (playerNum == 2) return isP2PayTag(uid, len);
  return false;
}

// ── BLE message handler ───────────────────────────────────────
void handleBLERx(const char* msg) {
  Serial.printf("[RX] %s\n", msg);

  // WAIT / START
  if (strcmp(msg, "WAIT") == 0) {
    S.gameStarted = false;
    curScreen = SCR_LOBBY; needRedraw = true; return;
  }
  if (strcmp(msg, "START") == 0) {
    S.gameStarted = true;
    // Keep showing lobby until TURN message
    curScreen = SCR_LOBBY; needRedraw = true; return;
  }
  if (strcmp(msg, "RESTART") == 0) {
    // Full reset: clear local state and return to lobby
    S.myBal = 1500; S.otherBal = 1500;
    S.myPos = 0; S.otherPos = 0;
    S.myTurn = false; S.inJail = false;
    S.lastDice = 0;
    S.action = LocalState::ACT_NONE;
    S.proxyPayFor = 0;
    S.auctionActive = false;
    S.gameOver = false;
    S.gameStarted = true;
    curScreen = SCR_LOBBY; needRedraw = true; return;
  }

  // TURN:{n}
  if (strncmp(msg, "TURN:", 5) == 0) {
    int n = atoi(msg+5);
    S.myTurn  = (n == PLAYER_NUM);
    S.lastDice = 0;
    S.action   = LocalState::ACT_NONE;
    S.proxyPayFor = 0;
    if (S.myTurn) {
      curScreen = SCR_HOME;
    } else {
      snprintf(S.actionTitle, sizeof(S.actionTitle), "Player %d's Turn", n);
      S.actionDesc[0] = '\0';
      curScreen = SCR_WAITING;
    }
    needRedraw = true; return;
  }

  // DICE:{sum}
  if (strncmp(msg, "DICE:", 5) == 0) {
    S.lastDice = atoi(msg+5);
    int newPos = (S.myPos + S.lastDice) % NUM_SQ;
    if (S.myTurn) {
      S.myPos = newPos;
      // Skip NFC confirmation screen — show result and wait for ACT message
      snprintf(S.actionTitle, sizeof(S.actionTitle), "Rolled %d!", S.lastDice);
      snprintf(S.actionDesc, sizeof(S.actionDesc),
        "Moved to:\n%s\nWaiting...", SQ_NAMES[newPos]);
      curScreen = SCR_WAITING;
    } else {
      S.otherPos = newPos;
      snprintf(S.actionTitle, sizeof(S.actionTitle), "Rolled %d", S.lastDice);
      snprintf(S.actionDesc, sizeof(S.actionDesc),
        "Other player\nmoves to:\n%s", SQ_NAMES[newPos]);
      curScreen = SCR_WAITING;
    }
    needRedraw = true; return;
  }

  // STATE:{p1pos}:{p1bal}:{p2pos}:{p2bal}
  if (strncmp(msg, "STATE:", 6) == 0) {
    int p1pos, p1bal, p2pos, p2bal;
    if (sscanf(msg+6, "%d:%d:%d:%d", &p1pos, &p1bal, &p2pos, &p2bal) == 4) {
      if (MY_IDX == 0) {
        S.myPos  = p1pos; S.myBal  = p1bal;
        S.otherPos = p2pos; S.otherBal = p2bal;
      } else {
        S.myPos  = p2pos; S.myBal  = p2bal;
        S.otherPos = p1pos; S.otherBal = p1bal;
      }
    }
    needRedraw = true; return;
  }

  if (strncmp(msg, "OWNERS:", 7) == 0) {
    const char* own = msg + 7;
    for (int i = 0; i < NUM_SQ && own[i]; i++) {
      if (own[i] == '1') sqOwnerLocal[i] = 1;
      else if (own[i] == '2') sqOwnerLocal[i] = 2;
      else sqOwnerLocal[i] = 0;
    }
    needRedraw = true; return;
  }

  // DONE
  if (strcmp(msg, "DONE") == 0) {
    S.proxyPayFor = 0;
    // If we were waiting to pay (rent/tax), show success feedback
    if (S.action == LocalState::ACT_RENT || S.action == LocalState::ACT_TAX) {
      strncpy(S.actionTitle, "PAYMENT OK!", 23);
      strncpy(S.actionDesc, "Payment complete!", 47);
      S.action = LocalState::ACT_INFO;
      curScreen = SCR_WAITING;
    }
    needRedraw = true; return;
  }

  // ACT:*  — only shown to current-screen player where relevant
  if (strncmp(msg, "ACT:", 4) == 0) {
    const char* act = msg + 4;

    if (strncmp(act, "BUY:", 4) == 0) {
      // ACT:BUY:{name}:{price}
      char tmp[64]; strncpy(tmp, act+4, 63); tmp[63] = '\0';
      char* colon = strrchr(tmp, ':');
      if (colon) {
        *colon = '\0';
        S.actionAmt = atoi(colon+1);
        strncpy(S.actionTitle, tmp, 23); S.actionTitle[23] = '\0';
      }
      if (S.myTurn) {
        snprintf(S.actionDesc, sizeof(S.actionDesc),
          "Price: $%d\nBalance: $%d\nAfter: $%d",
          S.actionAmt, S.myBal, S.myBal - S.actionAmt);
        S.action  = LocalState::ACT_BUY;
        curScreen = SCR_ACTION;
      } else {
        snprintf(S.actionDesc, sizeof(S.actionDesc),
          "Other player buying\n%s for $%d",
          S.actionTitle, S.actionAmt);
        curScreen = SCR_WAITING;
      }
      needRedraw = true; return;
    }

    if (strncmp(act, "RENT:", 5) == 0) {
      // ACT:RENT:{amt}:{who}
      char tmp[48]; strncpy(tmp, act+5, 47); tmp[47] = '\0';
      char* colon = strchr(tmp, ':');
      int rent = atoi(tmp);
      if (S.myTurn) {
        S.proxyPayFor = 0;
        strncpy(S.actionTitle, "PAY RENT", 23);
        snprintf(S.actionDesc, sizeof(S.actionDesc),
          "$%d to %s\nTap P%d reader\nor OK to pay",
          rent, colon ? colon+1 : "?", PLAYER_NUM == 1 ? 2 : 1);
        S.actionAmt = rent;
        S.action    = LocalState::ACT_RENT;
        curScreen   = SCR_ACTION;
      } else {
        snprintf(S.actionTitle, sizeof(S.actionTitle), "RENT COLLECTED");
        snprintf(S.actionDesc, sizeof(S.actionDesc),
          "Received $%d rent", rent);
        S.action  = LocalState::ACT_INFO;
        S.proxyPayFor = (PLAYER_NUM == 1) ? 2 : 1;
        curScreen = SCR_WAITING;
      }
      needRedraw = true; return;
    }

    if (strncmp(act, "TAX:", 4) == 0) {
      int tax = atoi(act+4);
      S.proxyPayFor = 0;
      strncpy(S.actionTitle, "INCOME TAX", 23);
      snprintf(S.actionDesc, sizeof(S.actionDesc),
        "Pay $%d tax\nBalance: $%d\nAfter: $%d",
        tax, S.myBal, S.myBal-tax);
      if (S.myTurn) {
        S.actionAmt = tax;
        S.action    = LocalState::ACT_TAX;
        curScreen   = SCR_ACTION;
      } else {
        S.action  = LocalState::ACT_INFO;
        curScreen = SCR_WAITING;
      }
      needRedraw = true; return;
    }

    if (strncmp(act, "GO:", 3) == 0) {
      int amt = atoi(act+3);
      strncpy(S.actionTitle, "LANDED ON GO!", 23);
      snprintf(S.actionDesc, sizeof(S.actionDesc),
        "Collect $%d!", amt);
      S.action = LocalState::ACT_INFO;
      curScreen = S.myTurn ? SCR_ACTION : SCR_WAITING;
      needRedraw = true; return;
    }

    if (strcmp(act, "OWN") == 0) {
      strncpy(S.actionTitle, "YOUR PROPERTY", 23);
      strncpy(S.actionDesc, "No action needed", 47);
      S.action = LocalState::ACT_INFO;
      curScreen = S.myTurn ? SCR_ACTION : SCR_WAITING;
      needRedraw = true; return;
    }

    if (strcmp(act, "JAIL") == 0) {
      S.inJail = S.myTurn;
      strncpy(S.actionTitle, "GO TO JAIL!", 23);
      strncpy(S.actionDesc, "Do not pass GO.\nPay $50 next turn.", 47);
      S.action = LocalState::ACT_INFO;
      curScreen = S.myTurn ? SCR_ACTION : SCR_WAITING;
      needRedraw = true; return;
    }

    if (strcmp(act, "VISIT") == 0) {
      strncpy(S.actionTitle, "Just Visiting", 23);
      strncpy(S.actionDesc, "Not in Jail.", 47);
      S.action = LocalState::ACT_INFO;
      curScreen = S.myTurn ? SCR_ACTION : SCR_WAITING;
      needRedraw = true; return;
    }

    if (strcmp(act, "FREE") == 0) {
      strncpy(S.actionTitle, "Free Parking!", 23);
      strncpy(S.actionDesc, "Rest here. Nothing\nhappens.", 47);
      S.action = LocalState::ACT_INFO;
      curScreen = S.myTurn ? SCR_ACTION : SCR_WAITING;
      needRedraw = true; return;
    }

    if (strncmp(act, "CHANCE:", 7) == 0) {
      strncpy(S.actionTitle, "CHANCE CARD", 23);
      strncpy(S.actionDesc, act+7, 47); S.actionDesc[47] = '\0';
      S.action = LocalState::ACT_INFO;
      curScreen = S.myTurn ? SCR_ACTION : SCR_WAITING;
      needRedraw = true; return;
    }

    if (strncmp(act, "CHEST:", 6) == 0) {
      strncpy(S.actionTitle, "COMMUNITY CHEST", 23);
      strncpy(S.actionDesc, act+6, 47); S.actionDesc[47] = '\0';
      S.action = LocalState::ACT_INFO;
      curScreen = S.myTurn ? SCR_ACTION : SCR_WAITING;
      needRedraw = true; return;
    }

    if (strncmp(act, "BAIL:", 5) == 0) {
      strncpy(S.actionTitle, "JAIL BAIL", 23);
      snprintf(S.actionDesc, sizeof(S.actionDesc),
        "Paid $%s to get\nout of jail.", act+5);
      S.inJail  = false;
      S.action  = LocalState::ACT_INFO;
      curScreen = S.myTurn ? SCR_ACTION : SCR_WAITING;
      needRedraw = true; return;
    }
  }

  // AUCTION:{sq}:{startPrice} — auction started
  if (strncmp(msg, "AUCTION:", 8) == 0) {
    const char* ap = msg + 8;

    // AUCTION:WIN:{n}:{sq}:{price}
    if (strncmp(ap, "WIN:", 4) == 0) {
      int winner, sq, price;
      if (sscanf(ap + 4, "%d:%d:%d", &winner, &sq, &price) == 3) {
        S.auctionActive = false;
        snprintf(S.actionTitle, sizeof(S.actionTitle),
          "AUCTION WON");
        snprintf(S.actionDesc, sizeof(S.actionDesc),
          "P%d wins\n%s\nfor $%d",
          winner, (sq >= 0 && sq < NUM_SQ) ? SQ_NAMES[sq] : "?", price);
        S.action  = LocalState::ACT_INFO;
        curScreen = SCR_ACTION;
      }
      needRedraw = true; return;
    }

    // AUCTION:NOBID
    if (strcmp(ap, "NOBID") == 0) {
      S.auctionActive = false;
      snprintf(S.actionTitle, sizeof(S.actionTitle), "NO BIDS");
      snprintf(S.actionDesc,  sizeof(S.actionDesc),
        "%s stays\nunowned.",
        (S.auctionSq >= 0 && S.auctionSq < NUM_SQ) ? SQ_NAMES[S.auctionSq] : "?");
      S.action  = LocalState::ACT_INFO;
      curScreen = SCR_ACTION;
      needRedraw = true; return;
    }

    // AUCTION:{sq}:{startPrice} — new auction begins
    int sq, startPrice;
    if (sscanf(ap, "%d:%d", &sq, &startPrice) == 2) {
      S.auctionActive     = true;
      S.auctionSq         = sq;
      S.auctionStartPrice = startPrice;
      S.auctionBid        = 0;
      S.auctionBidder     = 0;
      S.auctionStartMs    = millis();
      // Default bid = half list price, minimum $1
      S.myBidAmt          = max(1, startPrice / 2);
      curScreen = SCR_AUCTION;
      needRedraw = true; return;
    }
  }

  // BID:{playerNum}:{amt} — someone placed a bid
  if (strncmp(msg, "BID:", 4) == 0) {
    int who, amt;
    if (sscanf(msg + 4, "%d:%d", &who, &amt) == 2) {
      S.auctionBid    = amt;
      S.auctionBidder = who;
      S.auctionStartMs = millis();  // sync countdown with central timer reset
      // Auto-bump my pending bid above current if needed
      if (S.myBidAmt <= amt) {
        S.myBidAmt = amt + 10;
        if (S.myBidAmt > S.myBal) S.myBidAmt = S.myBal;
      }
      // Bring back to auction screen so player can counter-bid
      if (S.auctionActive) curScreen = SCR_AUCTION;
    }
    needRedraw = true; return;
  }

  // BANKRUPT:{n}
  if (strncmp(msg, "BANKRUPT:", 9) == 0) {
    int n = atoi(msg+9);
    S.gameOver = true;
    S.action   = (n == PLAYER_NUM) ? LocalState::ACT_LOSE : LocalState::ACT_WIN;
    curScreen  = SCR_GAME_OVER;
    needRedraw = true; return;
  }

  // WIN:{n}
  if (strncmp(msg, "WIN:", 4) == 0) {
    int n = atoi(msg+4);
    S.gameOver = true;
    S.action   = (n == PLAYER_NUM) ? LocalState::ACT_WIN : LocalState::ACT_LOSE;
    curScreen  = SCR_GAME_OVER;
    needRedraw = true; return;
  }
}

// ── Draw helpers ──────────────────────────────────────────────
void drawHeader(const char* title, uint16_t bg = C_BLUE) {
  display.fillRect(0, 0, 128, 14, bg);
  display.setTextColor(C_WHITE); display.setTextSize(1);
  display.setCursor(3, 3);
  display.print(title);
  // Player badge top-right
  char badge[6]; snprintf(badge, sizeof(badge), " P%d", PLAYER_NUM);
  display.setCursor(128 - 24, 3);
  display.print(badge);
}

// Print text with max char-width wrapping (for small OLED)
void printWrapped(int x, int& y, int lineH, int maxW, const char* txt) {
  const int CPERLINE = maxW / 6;  // textSize=1: 6px per char
  int len = strlen(txt);
  for (int i = 0; i < len; i += CPERLINE) {
    char row[CPERLINE+1];
    int take = min(CPERLINE, len-i);
    // break at newline if present
    const char* nl = strchr(txt+i, '\n');
    if (nl && (nl - (txt+i)) < take) take = nl - (txt+i);
    strncpy(row, txt+i, take); row[take] = '\0';
    display.setCursor(x, y); display.print(row);
    y += lineH;
    // skip newline char
    if (nl && (nl - (txt+i)) < take+1) i += (nl - (txt+i) + 1) - CPERLINE;
  }
}

// ── Screen renderers ──────────────────────────────────────────
void drawBoot() {
  display.fillScreen(C_BLACK);
  display.setTextColor(C_GOLD); display.setTextSize(2);
  display.setCursor(10, 28);
  display.print("MONOPOLY");
  display.setTextColor(C_LGRAY); display.setTextSize(1);
  display.setCursor(14, 54);
  display.printf("Player %d", PLAYER_NUM);
  display.setCursor(4, 70);
  display.setTextColor(S.bleConnected ? C_GREEN : C_GRAY);
  display.print(S.bleConnected ? "BLE: Connected" : "BLE: Scanning...");
  display.setTextColor(C_DKGRAY); display.setCursor(4, 88);
  display.print("Looking for:");
  display.setCursor(4, 100);
  display.setTextColor(C_CYAN);
  display.print(BLE_SERVER_NAME);
}

void drawLobby() {
  display.fillScreen(C_BLACK);
  drawHeader("LOBBY", C_PURPLE);
  display.setTextColor(C_GREEN); display.setTextSize(1);
  display.setCursor(4, 20);
  display.print("Connected!");
  display.setTextColor(C_WHITE); display.setCursor(4, 34);
  display.printf("You are Player %d", PLAYER_NUM);
  display.setTextColor(C_GRAY); display.setCursor(4, 50);
  display.print("Waiting for game");
  display.setCursor(4, 62);
  display.print("to start...");
  display.setTextColor(C_DKGRAY); display.setCursor(4, 90);
  display.print("Balance: $1500");
  display.setCursor(4, 102);
  display.print("Position: GO");
}

void drawHome() {
  display.fillScreen(C_BLACK);
  uint16_t hCol = S.myTurn ? C_GREEN : C_DKGRAY;
  drawHeader(S.myTurn ? "YOUR TURN!" : "MONOPOLY", hCol);

  display.setTextSize(1);

  // Balance
  display.setTextColor(S.myBal >= 0 ? C_GREEN : C_RED);
  display.setCursor(4, 18);
  display.printf("Bal: $%d", S.myBal);

  // Position
  display.setTextColor(C_WHITE);
  display.setCursor(4, 30);
  char posStr[22]; snprintf(posStr, sizeof(posStr), "@ %s", SQ_NAMES[S.myPos]);
  display.print(posStr);

  // Other player
  display.setTextColor(P_COLOR[1 - MY_IDX]);
  display.setCursor(4, 44);
  display.printf("P%d: $%d @ %s", PLAYER_NUM==1 ? 2 : 1,
    S.otherBal, SQ_NAMES[S.otherPos]);

  // Dice result
  if (S.lastDice > 0) {
    display.setTextColor(C_YELLOW); display.setCursor(4, 58);
    display.printf("Rolled: %d", S.lastDice);
  }

  // Status
  if (S.inJail) {
    display.setTextColor(C_RED); display.setCursor(4, 72);
    display.print("IN JAIL");
  }

  // Instructions
  display.setTextColor(C_DKGRAY); display.setCursor(4, 108);
  if (S.myTurn) {
    display.print("[OK]:Roll dice");
  } else {
    display.print("[waiting...]");
  }
}

void drawNFCScreen() {
  display.fillScreen(C_BLACK);
  drawHeader("MOVE PIECE", C_ORANGE);

  display.setTextColor(C_YELLOW); display.setTextSize(1);
  display.setCursor(4, 18);
  display.printf("Rolled: %d", S.lastDice);

  display.setTextColor(C_WHITE); display.setCursor(4, 30);
  display.print("Move to:");
  display.setTextColor(C_CYAN); display.setCursor(4, 42);
  display.print(SQ_NAMES[S.myPos]);

  display.setTextColor(C_GRAY); display.setCursor(4, 60);
  display.print("Scan piece to");
  display.setCursor(4, 72);
  display.print("confirm position");
  display.setCursor(4, 84);
  display.print("-- or --");
  display.setCursor(4, 96);
  display.print("Press OK to skip");

  display.setTextColor(C_DKGRAY); display.setCursor(4, 116);
  display.print("[L]:Cancel");
}

void drawAction() {
  display.fillScreen(C_BLACK);

  // Header color by action type
  uint16_t hCol = C_BLUE;
  if (S.action == LocalState::ACT_BUY)  hCol = C_GREEN;
  if (S.action == LocalState::ACT_RENT) hCol = C_RED;
  if (S.action == LocalState::ACT_TAX)  hCol = C_RED;

  drawHeader(S.actionTitle, hCol);

  display.setTextColor(C_WHITE); display.setTextSize(1);
  int y = 18;
  // Split actionDesc by \n
  char tmp[48]; strncpy(tmp, S.actionDesc, 47); tmp[47] = '\0';
  char* line = strtok(tmp, "\n");
  while (line && y < 96) {
    display.setCursor(4, y);
    display.print(line);
    y += 12;
    line = strtok(nullptr, "\n");
  }

  // Action buttons
  display.setTextColor(C_DKGRAY); display.setCursor(4, 110);
  switch (S.action) {
    case LocalState::ACT_BUY:
      display.setTextColor(C_GREEN);
      display.print("[OK]:BUY");
      display.setTextColor(C_DKGRAY);
      display.setCursor(70, 110);
      display.print("[R]:Skip");
      break;
    case LocalState::ACT_RENT:
      display.setTextColor(C_YELLOW);
      display.print("Tap to pay");
      display.setCursor(4, 120);
      display.setTextColor(C_CYAN);
      display.print("or [OK]:Pay");
      break;
    case LocalState::ACT_TAX:
      display.setTextColor(C_YELLOW);
      display.print("[OK]:OK");
      break;
    case LocalState::ACT_INFO:
      display.setTextColor(C_GRAY);
      display.print("[OK]:Continue");
      break;
    default:
      break;
  }
}

void drawAuction() {
  display.fillScreen(C_BLACK);
  drawHeader("AUCTION", C_ORANGE);

  display.setTextSize(1);

  // Property name
  display.setTextColor(C_CYAN); display.setCursor(4, 18);
  if (S.auctionSq >= 0 && S.auctionSq < NUM_SQ)
    display.print(SQ_NAMES[S.auctionSq]);

  // List price
  display.setTextColor(C_GRAY); display.setCursor(4, 30);
  display.printf("List: $%d", S.auctionStartPrice);

  // Current highest bid
  display.setTextColor(C_YELLOW); display.setCursor(4, 44);
  if (S.auctionBid > 0) {
    display.printf("High bid: $%d (P%d)", S.auctionBid, S.auctionBidder);
  } else {
    display.print("No bids yet");
  }

  // Separator
  display.drawFastHLine(4, 56, 120, C_DKGRAY);

  // My bid amount
  display.setTextColor(C_WHITE); display.setCursor(4, 62);
  display.print("Your bid:");
  display.setTextColor(C_GREEN); display.setTextSize(2);
  display.setCursor(4, 74);
  display.printf("$%d", S.myBidAmt);

  display.setTextSize(1);
  // Balance after bid
  display.setTextColor(C_GRAY); display.setCursor(4, 94);
  display.printf("After: $%d", S.myBal - S.myBidAmt);

  // Countdown timer
  {
    uint32_t el = millis() - S.auctionStartMs;
    int secs = (int)((60000 - el) / 1000);
    if (secs < 0) secs = 0;
    display.setCursor(90, 18);
    display.setTextColor(secs <= 10 ? C_RED : C_YELLOW);
    display.printf("%ds", secs);
  }

  // Button hints
  display.setTextColor(C_GREEN); display.setCursor(4, 110);
  display.print("[OK]:Bid");
  display.setTextColor(C_DKGRAY); display.setCursor(54, 110);
  display.print("[U]:Props");
  display.setTextColor(C_GRAY); display.setCursor(4, 120);
  display.print("[L/R]:+/-$10");
}

void drawWaiting() {
  bool isPaymentOK = (strcmp(S.actionTitle, "PAYMENT OK!") == 0);
  display.fillScreen(C_BLACK);

  if (isPaymentOK) {
    // Big green success screen
    drawHeader("PAYMENT OK!", C_GREEN);
    display.setTextColor(C_GREEN); display.setTextSize(2);
    display.setCursor(8, 30);
    display.print("SUCCESS!");
    display.setTextColor(C_WHITE); display.setTextSize(1);
    int y = 58;
    char tmp[48]; strncpy(tmp, S.actionDesc, 47); tmp[47] = '\0';
    char* line = strtok(tmp, "\n");
    while (line && y < 96) {
      display.setCursor(4, y); display.print(line);
      y += 12;
      line = strtok(nullptr, "\n");
    }
    display.setTextColor(C_DKGRAY); display.setCursor(4, 110);
    display.print("[OK]:Continue");
    return;
  }

  drawHeader(S.actionTitle[0] ? S.actionTitle : "WAITING...", C_DKGRAY);

  display.setTextColor(C_GRAY); display.setTextSize(1);
  int y = 20;
  char tmp[48]; strncpy(tmp, S.actionDesc, 47); tmp[47] = '\0';
  char* line = strtok(tmp, "\n");
  while (line && y < 96) {
    display.setCursor(4, y); display.print(line);
    y += 12;
    line = strtok(nullptr, "\n");
  }

  if (y < 96) {
    if (S.proxyPayFor > 0) {
      display.setTextColor(C_CYAN); display.setCursor(4, 100);
      display.printf("Tap P%d tag", S.proxyPayFor);
      display.setTextColor(C_DKGRAY); display.setCursor(4, 112);
      display.print("or [OK]:Confirm");
    } else {
      display.setTextColor(C_DKGRAY); display.setCursor(4, 100);
      display.printf("P%d: $%d", PLAYER_NUM==1 ? 2 : 1, S.otherBal);
      display.setCursor(4, 112);
      display.printf("@ %s", SQ_NAMES[S.otherPos]);
    }
  }
}

void drawGameOver() {
  display.fillScreen(C_BLACK);
  bool win = (S.action == LocalState::ACT_WIN);
  uint16_t col = win ? C_GOLD : C_RED;

  drawHeader(win ? "YOU WIN!" : "YOU LOSE", win ? C_GREEN : C_RED);

  display.setTextColor(col); display.setTextSize(2);
  display.setCursor(win ? 20 : 14, 36);
  display.print(win ? "WINNER!" : "BANKRUPT");

  display.setTextColor(C_WHITE); display.setTextSize(1);
  display.setCursor(4, 72);
  display.printf("Final: $%d", S.myBal);
  display.setCursor(4, 86);
  display.printf("P%d: $%d", PLAYER_NUM==1 ? 2 : 1, S.otherBal);
}

void drawProperties() {
  display.fillScreen(C_BLACK);
  drawHeader("MY PROPERTIES", C_PURPLE);

  int owned[NUM_SQ];
  int count = 0;
  for (int i = 0; i < NUM_SQ; i++) {
    if (sqOwnerLocal[i] == PLAYER_NUM && SQ_GRP_NAMES[i][0]) {
      owned[count++] = i;
    }
  }

  if (count == 0) {
    display.setTextColor(C_GRAY); display.setTextSize(1);
    display.setCursor(4, 42);
    display.print("No properties yet");
    display.setCursor(4, 116);
    display.print("[L]:Back");
    return;
  }

  if (propScrollOffset > max(0, count - 4)) propScrollOffset = max(0, count - 4);

  for (int r = 0; r < 4; r++) {
    int idx = propScrollOffset + r;
    if (idx >= count) break;
    int sq = owned[idx];
    int y = 18 + r * 24;
    display.fillRect(4, y + 2, 12, 12, SQ_GRP_COLORS[sq]);
    display.drawRect(4, y + 2, 12, 12, C_WHITE);
    display.setTextColor(C_WHITE); display.setTextSize(1);
    display.setCursor(20, y);
    char nm[16];
    strncpy(nm, SQ_NAMES[sq], 15);
    nm[15] = '\0';
    display.print(nm);
  }

  display.setTextColor(C_DKGRAY); display.setCursor(4, 116);
  display.print("[U]:Next [L]:Back");
}

void redrawScreen() {
  switch (curScreen) {
    case SCR_BOOT:      drawBoot();      break;
    case SCR_LOBBY:     drawLobby();     break;
    case SCR_HOME:      drawHome();      break;
    case SCR_NFC:       drawNFCScreen(); break;
    case SCR_ACTION:    drawAction();    break;
    case SCR_WAITING:   drawWaiting();   break;
    case SCR_AUCTION:   drawAuction();   break;
    case SCR_PROPERTIES: drawProperties(); break;
    case SCR_GAME_OVER: drawGameOver();  break;
  }
}

// ── Button event handler ──────────────────────────────────────
void handleButton(uint8_t pin, BtnEvt evt) {
  if (evt == EVT_NONE) return;
  Serial.printf("[BTN] pin %d %s\n", pin, evt==EVT_SHORT?"SHORT":"LONG");

  if (!S.bleConnected) return;   // nothing to do if not connected

  if (pin == BTN_UP && evt == EVT_SHORT &&
      curScreen != SCR_PROPERTIES &&
      curScreen != SCR_BOOT &&
      curScreen != SCR_LOBBY &&
      curScreen != SCR_GAME_OVER) {
    returnScreen = curScreen;
    propScrollOffset = 0;
    curScreen = SCR_PROPERTIES;
    needRedraw = true;
    return;
  }

  switch (curScreen) {

    case SCR_HOME:
      if (pin == BTN_OK && evt == EVT_SHORT && S.myTurn) {
        sendCmd("ROLL");
        // Show immediate feedback so player knows the roll was sent
        strncpy(S.actionTitle, "Rolling...", 23);
        strncpy(S.actionDesc,  "Waiting for\ndice result...", 47);
        curScreen  = SCR_WAITING;
        needRedraw = true;
      }
      break;

    case SCR_NFC:
      // OK → skip NFC, proceed (central already computed position from dice)
      if (pin == BTN_OK && evt == EVT_SHORT) {
        // Nothing to send — central processes the square automatically
        // after the 2s pause in runDiceAnim; player just waits for ACT msg
        curScreen  = SCR_WAITING;
        strncpy(S.actionTitle, "Processing...", 23);
        S.actionDesc[0] = '\0';
        needRedraw = true;
      }
      if (pin == BTN_LEFT && evt == EVT_SHORT) {
        // Cancel: go back to home (central still processes; this is just UI)
        curScreen  = SCR_HOME;
        needRedraw = true;
      }
      break;

    case SCR_ACTION:
      if (pin == BTN_OK && evt == EVT_SHORT) {
        switch (S.action) {
          case LocalState::ACT_BUY:  sendCmd("BUY");  break;
          case LocalState::ACT_RENT: sendCmd("OK");   break;
          case LocalState::ACT_TAX:  sendCmd("OK");   break;
          case LocalState::ACT_INFO: sendCmd("OK");   break;
          default: break;
        }
        curScreen = SCR_HOME; needRedraw = true;
      }
      if (pin == BTN_RIGHT && evt == EVT_SHORT && S.action == LocalState::ACT_BUY) {
        sendCmd("SKIP");
        curScreen = SCR_HOME; needRedraw = true;
      }
      if (pin == BTN_LEFT && evt == EVT_SHORT) {
        curScreen = SCR_HOME; needRedraw = true;
      }
      break;

    case SCR_AUCTION:
      if (pin == BTN_RIGHT && (evt == EVT_SHORT || evt == EVT_LONG)) {
        S.myBidAmt += 10;
        if (S.myBidAmt > S.myBal) S.myBidAmt = S.myBal;
        needRedraw = true;
      }
      if (pin == BTN_LEFT && (evt == EVT_SHORT || evt == EVT_LONG)) {
        S.myBidAmt -= 10;
        int minBid = S.auctionBid + 1;  // must exceed current highest
        if (minBid < 1) minBid = 1;
        if (S.myBidAmt < minBid) S.myBidAmt = minBid;
        needRedraw = true;
      }
      if (pin == BTN_OK && evt == EVT_SHORT) {
        // Send bid — must be > current highest and <= my balance
        if (S.myBidAmt > S.auctionBid && S.myBidAmt <= S.myBal) {
          char buf[16];
          snprintf(buf, sizeof(buf), "BID:%d", S.myBidAmt);
          sendCmd(buf);
          // Show feedback
          strncpy(S.actionTitle, "Bid sent!", 23);
          snprintf(S.actionDesc, sizeof(S.actionDesc),
            "You bid $%d\nWaiting...", S.myBidAmt);
          curScreen = SCR_WAITING;
          needRedraw = true;
        }
      }
      if (pin == BTN_UP && evt == EVT_SHORT) {
        // Pass — don't bid
        sendCmd("PASS");
        strncpy(S.actionTitle, "Passed", 23);
        strncpy(S.actionDesc, "Auction continues...", 47);
        curScreen = SCR_WAITING;
        needRedraw = true;
      }
      break;

    case SCR_WAITING:
      // Home shortcut: show own status
      if (pin == BTN_OK && evt == EVT_SHORT) {
        if (S.proxyPayFor > 0) {
          int payFor = S.proxyPayFor;
          sendPlayerCmd(payFor, "OK");
          S.proxyPayFor = 0;
          strncpy(S.actionTitle, "PAYMENT OK!", 23);
          snprintf(S.actionDesc, sizeof(S.actionDesc),
            "Rent received\nfrom P%d", payFor);
          curScreen = SCR_WAITING;
        } else {
          curScreen = SCR_HOME;
        }
        needRedraw = true;
      }
      break;

    case SCR_PROPERTIES: {
      if (pin == BTN_LEFT && evt == EVT_SHORT) {
        curScreen = returnScreen;
        needRedraw = true;
      }
      if (pin == BTN_UP && evt == EVT_SHORT) {
        int count = 0;
        for (int i = 0; i < NUM_SQ; i++) {
          if (sqOwnerLocal[i] == PLAYER_NUM && SQ_GRP_NAMES[i][0]) count++;
        }
        int maxScroll = max(0, count - 4);
        propScrollOffset = (propScrollOffset >= maxScroll) ? 0 : propScrollOffset + 1;
        needRedraw = true;
      }
      break;
    }

    default:
      break;
  }
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis()-t0 < 3000) delay(10);
  delay(100);
  Serial.println("[BOOTMARK] player_card safe boot v1");
  Serial.printf("[BOOT] Monopoly Player %d\n", PLAYER_NUM);

  // OLED
  Serial.println("[OLED] SPI.begin...");
  SPI.begin(D8, D9, D10, OLED_CS);
  Serial.println("[OLED] display.begin()...");
  display.begin();
  Serial.println("[OLED] fillScreen...");
  display.setRotation(0);
  display.setTextWrap(false);
  display.fillScreen(C_RED);    // ← 先填红色，比黑色更容易看出有没有反应
  delay(500);
  display.fillScreen(C_BLACK);
  display.setTextColor(C_GREEN); display.setTextSize(1);
  display.setCursor(4, 4);  display.print("OLED OK");
  Serial.println("[OLED] init done");

  // NFC (I2C)
  Wire.begin();
  static Adafruit_PN532 nfcObj(BTN_LEFT, BTN_RIGHT);
  nfc = &nfcObj;
  nfc->begin();
  display.setCursor(4, 18); display.setTextColor(C_YELLOW);
  display.print("NFC init...");
  uint32_t fw = nfc->getFirmwareVersion();
  Serial.printf("[NFC] firmware=0x%08lX\n", (unsigned long)fw);
  if (fw) {
    nfc->SAMConfig();
    nfcAvail = true;
    display.setCursor(4, 30); display.setTextColor(C_GREEN);
    display.print("NFC OK");
  } else {
    display.setCursor(4, 30); display.setTextColor(C_RED);
    display.print("NFC: not found");
  }

  // Buttons (4 buttons only — D4 is I2C SDA, not available)
  initButtons();

  // Initial state
  memset(&S, 0, sizeof(S));
  S.myBal    = 1500;
  S.otherBal = 1500;
  S.action   = LocalState::ACT_NONE;
  S.auctionSq = -1;
  for (int i = 0; i < NUM_SQ; i++) sqOwnerLocal[i] = 0;

  // BLE
  display.setCursor(4, 44); display.setTextColor(C_CYAN);
  display.print("BLE scanning...");
  setupBLE();

  delay(800);
  curScreen  = SCR_BOOT;
  needRedraw = true;
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  // BLE: reconnect when server found
  if (pServer && !S.bleConnected) {
    display.fillScreen(C_BLACK);
    display.setTextColor(C_CYAN); display.setTextSize(1);
    display.setCursor(4, 50);
    display.print("BLE Connecting...");
    if (!bleDoConnect()) {
      NimBLEDevice::getScan()->start(10, nullptr, false);
    }
  }

  // BLE: restart scan if disconnected and not scanning
  if (!S.bleConnected && !pServer &&
      !NimBLEDevice::getScan()->isScanning()) {
    NimBLEDevice::getScan()->start(10, nullptr, false);
  }

  // Process incoming BLE notifies (drain queue — handles rapid back-to-back msgs)
  while (qHead != qTail) {
    uint8_t h = qHead;
    qHead = (qHead + 1) % BLE_Q_SIZE;
    bool keepPropertiesOpen = (curScreen == SCR_PROPERTIES);
    handleBLERx(bleQueue[h]);
    if (keepPropertiesOpen && curScreen != SCR_GAME_OVER) {
      curScreen = SCR_PROPERTIES;
      needRedraw = true;
    }
  }

  // NFC scan (only active on SCR_NFC, non-blocking 60ms)
  bool isPayAction = false;
  bool isProxyPayAction = (curScreen == SCR_WAITING && S.proxyPayFor > 0);
  if (nfcAvail && (curScreen == SCR_NFC || isPayAction || isProxyPayAction)) {
    uint8_t uid[7]; uint8_t len = 0;
    if (nfc->readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 60)) {
      Serial.print("[NFC] UID: ");
      for (int i = 0; i < len; i++) Serial.printf("%02X ", uid[i]);
      Serial.println();
      if (isPayAction) {
        if (millis() - lastNfcPayMs > 1500) {
          lastNfcPayMs = millis();
          Serial.println("[NFC] Payment confirmed by tap");
          sendCmd("OK");
          curScreen = SCR_HOME;
          needRedraw = true;
        }
        return;
      }
      if (isProxyPayAction) {
        if (millis() - lastNfcPayMs > 1500) {
          lastNfcPayMs = millis();
          int payFor = S.proxyPayFor;
          if (!payTagMatches(payFor, uid, len)) {
            Serial.printf("[NFC] Wrong payment tag for P%d\n", payFor);
            strncpy(S.actionTitle, "WRONG TAG", 23);
            snprintf(S.actionDesc, sizeof(S.actionDesc),
              "Need P%d payment tag", payFor);
            curScreen = SCR_WAITING;
            needRedraw = true;
            return;
          }
          Serial.printf("[NFC] P%d payment confirmed via this reader\n", payFor);
          sendPlayerCmd(payFor, "OK");
          S.proxyPayFor = 0;
          strncpy(S.actionTitle, "PAYMENT OK!", 23);
          snprintf(S.actionDesc, sizeof(S.actionDesc),
            "Rent received\nfrom P%d", payFor);
          curScreen = SCR_WAITING;
          needRedraw = true;
        }
        return;
      }
      int sq = lookupNfcUID(uid, len);
      if (sq >= 0 && sq != S.myPos) {
        // NFC says different square — show mismatch but don't override
        // central's computed position (dice is authoritative)
        Serial.printf("[NFC] Scanned sq %d but expected %d\n", sq, S.myPos);
      }
      // Any successful scan: confirm position and proceed
      curScreen  = SCR_WAITING;
      strncpy(S.actionTitle, "Piece confirmed!", 23);
      snprintf(S.actionDesc, sizeof(S.actionDesc),
        "On: %s\nWaiting for\naction...", SQ_NAMES[S.myPos]);
      needRedraw = true;
    }
  }

  // Poll buttons
  for (int i = 0; i < 4; i++) {
    BtnEvt e = pollButton(BTNS[i]);
    if (e != EVT_NONE) handleButton(BTNS[i].pin, e);
  }

  // Force redraw every second on auction screen (for countdown)
  if (curScreen == SCR_AUCTION && !needRedraw) {
    static uint32_t lastAucRedraw = 0;
    uint32_t now = millis();
    if (now - lastAucRedraw >= 1000) {
      lastAucRedraw = now;
      needRedraw = true;
    }
  }

  // Redraw when needed
  if (needRedraw) {
    redrawScreen();
    needRedraw = false;
  }

  delay(16);
}
