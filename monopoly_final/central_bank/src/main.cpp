// ============================================================
// central_bank/src/main.cpp
// CrowPanel 5" ESP32-S3 800×480 — Monopoly Central Bank
// 2-player game master: full state, dice, BLE server
//
// BLE messages OUT (notify to all players):
//   WAIT                    — waiting for players
//   START                   — both connected, game starting
//   TURN:{n}                — player n's turn (1 or 2)
//   DICE:{sum}              — dice result
//   STATE:{p1pos}:{p1bal}:{p2pos}:{p2bal}
//   ACT:GO:{amt}            — landed/passed GO
//   ACT:BUY:{name}:{price}  — purchase option (current player only)
//   ACT:RENT:{amt}:{owner}  — pay rent
//   ACT:TAX:{amt}           — tax payment
//   ACT:OWN                 — own property, no action
//   ACT:JAIL                — sent to jail (via card)
//   ACT:VISIT               — just visiting jail
//   ACT:FREE                — free parking
//   ACT:CHANCE:{text}       — chance card drawn
//   ACT:CHEST:{text}        — community chest drawn
//   ACT:BAIL:{amt}          — player paid jail bail
//   DONE                    — turn ended, state updated
//   BANKRUPT:{n}            — player n bankrupt
//   WIN:{n}                 — player n wins
//   AUCTION:{sq}:{startPrice} — auction started
//   BID:{n}:{amt}           — player n bid amt (re-broadcast)
//   AUCTION:WIN:{n}:{sq}:{price} — auction winner confirmed
//   AUCTION:NOBID           — no bids, property stays unowned
//   TRADE:{from}:{sq_offer}:{sq_want} — trade proposal broadcast
//   TRADE:DONE              — trade executed
//   TRADE:FAIL              — trade rejected
//
// BLE messages IN (write from player):
//   P{n}:HELLO              — player registration
//   P{n}:ROLL               — request dice roll
//   P{n}:BUY                — buy current property
//   P{n}:SKIP               — decline to buy → triggers auction
//   P{n}:OK                 — confirm action (pay rent/tax/etc.)
//   P{n}:BID:{amt}          — place auction bid
//   P{n}:PASS               — pass on auction (no bid)
//   P{n}:TRADE:{sq_mine}:{sq_theirs} — propose trade
//   P{n}:TRADE:YES          — accept pending trade
//   P{n}:TRADE:NO           — reject pending trade
// ============================================================

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <Wire.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_now.h>

// ── LovyanGFX — CrowPanel 5" V1.1 (800×480, RGB parallel) ──
class LGFX : public lgfx::LGFX_Device {
public:
  lgfx::Bus_RGB     _bus_instance;
  lgfx::Panel_RGB   _panel_instance;
  lgfx::Touch_GT911 _touch_instance;

  LGFX(void) {
    { auto cfg = _panel_instance.config();
      cfg.memory_width  = cfg.panel_width  = 800;
      cfg.memory_height = cfg.panel_height = 480;
      _panel_instance.config(cfg); }

    { auto cfg = _panel_instance.config_detail();
      cfg.use_psram = 2;   // double-buffer via PSRAM
      _panel_instance.config_detail(cfg); }

    { auto cfg = _bus_instance.config();
      cfg.panel    = &_panel_instance;
      cfg.pin_d0   = GPIO_NUM_21;  cfg.pin_d1  = GPIO_NUM_47;
      cfg.pin_d2   = GPIO_NUM_48;  cfg.pin_d3  = GPIO_NUM_45;
      cfg.pin_d4   = GPIO_NUM_38;  cfg.pin_d5  = GPIO_NUM_9;
      cfg.pin_d6   = GPIO_NUM_10;  cfg.pin_d7  = GPIO_NUM_11;
      cfg.pin_d8   = GPIO_NUM_12;  cfg.pin_d9  = GPIO_NUM_13;
      cfg.pin_d10  = GPIO_NUM_14;  cfg.pin_d11 = GPIO_NUM_7;
      cfg.pin_d12  = GPIO_NUM_17;  cfg.pin_d13 = GPIO_NUM_18;
      cfg.pin_d14  = GPIO_NUM_3;   cfg.pin_d15 = GPIO_NUM_46;
      cfg.pin_henable     = GPIO_NUM_42;
      cfg.pin_vsync       = GPIO_NUM_41;
      cfg.pin_hsync       = GPIO_NUM_40;
      cfg.pin_pclk        = GPIO_NUM_39;
      cfg.freq_write      = 16000000;  // 21MHz → 16MHz: reduce PSRAM bandwidth pressure
      cfg.hsync_polarity  = 0; cfg.hsync_front_porch = 8;
      cfg.hsync_pulse_width = 4; cfg.hsync_back_porch = 8;
      cfg.vsync_polarity  = 0; cfg.vsync_front_porch = 8;
      cfg.vsync_pulse_width = 4; cfg.vsync_back_porch = 8;
      cfg.pclk_idle_high  = 1;
      _bus_instance.config(cfg); }
    _panel_instance.setBus(&_bus_instance);

    { auto cfg = _touch_instance.config();
      cfg.x_min = 0; cfg.x_max = 800;
      cfg.y_min = 0; cfg.y_max = 480;
      cfg.pin_int = -1; cfg.pin_rst = -1;
      cfg.bus_shared = true; cfg.offset_rotation = 0;
      cfg.i2c_port = 0;
      cfg.pin_sda  = GPIO_NUM_15; cfg.pin_scl = GPIO_NUM_16;
      cfg.freq     = 400000;      cfg.i2c_addr = 0x5D;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance); }

    setPanel(&_panel_instance);
  }
};
static LGFX display;

// ── STC8H1K28: backlight + buzzer ───────────────────────────
#define STC_ADDR 0x30
void stcSend(uint8_t cmd) {
  Wire.beginTransmission(STC_ADDR);
  Wire.write(cmd); Wire.endTransmission();
}
void waitI2CReady() {
  while (true) {
    Wire.beginTransmission(0x30); bool a = !Wire.endTransmission();
    Wire.beginTransmission(0x5D); bool b = !Wire.endTransmission();
    if (a && b) break;
    stcSend(0x19);
    pinMode(1, OUTPUT); digitalWrite(1, LOW); delay(120);
    pinMode(1, INPUT);  delay(100);
  }
}
void beep(int ms = 80) { stcSend(0x15); delay(ms); stcSend(0x16); }
inline void flushDisp() {
  display.endWrite();
  display.waitDisplay();   // wait for vsync / DMA swap before next frame
  display.startWrite();
}

// ── Colors ───────────────────────────────────────────────────
#define C_BG      0x0B0B22UL
#define C_WHITE   0xFFFFFFUL
#define C_BLACK   0x000000UL
#define C_RED     0xFF3333UL
#define C_BLUE    0x3366FFUL
#define C_GREEN   0x22CC44UL
#define C_YELLOW  0xFFDD00UL
#define C_ORANGE  0xFF8800UL
#define C_GRAY    0x777788UL
#define C_LGRAY   0xCCCCDDUL
#define C_GOLD    0xFFD700UL
#define C_CYAN    0x00EEEDUL
#define C_PURPLE  0xAA00DDUL
#define C_PINK    0xFF66AAUL
#define C_LBLUE   0x55BBFFUL
#define C_DKBLUE  0x0A0A2AUL

// Property group accent colors
#define GRP_NONE   0x2A2A4AUL
#define GRP_PURPLE 0x60B840UL  // visible green
#define GRP_LBLUE  0xE0C880UL  // warm tan (visible, not white, not pure yellow)
#define GRP_PINK   0x4A4A4AUL  // dark grey
#define GRP_ORANGE 0xA0522DUL  // sienna
#define GRP_RED    0xFF8800UL  // orange
#define GRP_YELLOW 0xFFD43BUL
#define GRP_GREEN  0xC09070UL  // light brown
#define GRP_DBLUE  0x2255CCUL

static const uint32_t P_COLOR[2] = { C_RED, C_BLUE };

// ── Board: 20 squares ────────────────────────────────────────
#define NUM_SQ     40
#define NUM_PLY     2
#define START_BAL 1500
#define GO_BONUS   200
#define JAIL_SQ     10

enum SqType {
  SQ_GO, SQ_PROPERTY, SQ_RAILROAD, SQ_UTILITY,
  SQ_TAX, SQ_CHANCE, SQ_CHEST, SQ_JAIL,
  SQ_GOTO_JAIL, SQ_FREE_PARK
};

struct Square {
  const char* name;    // display name (max ~12 chars for cell)
  SqType      type;
  int         price;   // 0 = not buyable
  int         value;   // rent, tax amount, or utility fixed rent
  uint32_t    grp;     // group color for property bar
};

// 20 squares arranged in a 4-column × 5-row grid on screen
static const Square SQ[NUM_SQ] = {
  // idx  name            type          price  value    grp
  { "GO",            SQ_GO,          0,    0,    GRP_NONE   },  //  0
  { "Old Kent Road", SQ_PROPERTY,   60,    2,    GRP_PURPLE },  //  1
  { "Community Chest", SQ_CHEST,    0,    0,    GRP_NONE   },  //  2
  { "Whitechapel Road", SQ_PROPERTY, 60,   4,    GRP_PURPLE },  //  3
  { "Income Tax",    SQ_TAX,         0,  200,    GRP_NONE   },  //  4
  { "King's Cross Station", SQ_RAILROAD, 200, 25, GRP_NONE   },  //  5
  { "The Angel Islington", SQ_PROPERTY, 100,  6,  GRP_LBLUE  },  //  6
  { "Chance",        SQ_CHANCE,      0,    0,    GRP_NONE   },  //  7
  { "Euston Road",   SQ_PROPERTY,  100,    6,    GRP_LBLUE  },  //  8
  { "Pentonville Road", SQ_PROPERTY, 120,  8,    GRP_LBLUE  },  //  9
  { "Jail / Just Visiting", SQ_JAIL, 0,    0,    GRP_NONE   },  // 10
  { "Pall Mall",     SQ_PROPERTY,  140,   10,    GRP_PINK   },  // 11
  { "Electric Company", SQ_UTILITY, 150,  75,    GRP_NONE   },  // 12
  { "Whitehall",     SQ_PROPERTY,  140,   10,    GRP_PINK   },  // 13
  { "Northumberland Avenue", SQ_PROPERTY, 160, 12, GRP_PINK },  // 14
  { "Marylebone Station", SQ_RAILROAD, 200, 25, GRP_NONE   },  // 15
  { "Bow Street",    SQ_PROPERTY,  180,   14,    GRP_ORANGE },  // 16
  { "Community Chest", SQ_CHEST,    0,    0,    GRP_NONE   },  // 17
  { "Marlborough Street", SQ_PROPERTY, 180, 14, GRP_ORANGE },  // 18
  { "Vine Street",   SQ_PROPERTY,  200,   16,    GRP_ORANGE },  // 19
  { "Free Parking",  SQ_FREE_PARK,   0,    0,    GRP_NONE   },  // 20
  { "Strand",        SQ_PROPERTY,  220,   18,    GRP_RED    },  // 21
  { "Chance",        SQ_CHANCE,      0,    0,    GRP_NONE   },  // 22
  { "Fleet Street",  SQ_PROPERTY,  220,   18,    GRP_RED    },  // 23
  { "Trafalgar Square", SQ_PROPERTY, 240, 20,    GRP_RED    },  // 24
  { "Fenchurch St. Station", SQ_RAILROAD, 200, 25, GRP_NONE },  // 25
  { "Leicester Square", SQ_PROPERTY, 260, 22,    GRP_YELLOW },  // 26
  { "Coventry Street", SQ_PROPERTY, 260,  22,    GRP_YELLOW },  // 27
  { "Water Works",   SQ_UTILITY,   150,   75,    GRP_NONE   },  // 28
  { "Piccadilly",    SQ_PROPERTY,  280,   24,    GRP_YELLOW },  // 29
  { "Go To Jail",    SQ_GOTO_JAIL,   0,    0,    GRP_NONE   },  // 30
  { "Regent Street", SQ_PROPERTY,  300,   26,    GRP_GREEN  },  // 31
  { "Oxford Street", SQ_PROPERTY,  300,   26,    GRP_GREEN  },  // 32
  { "Community Chest", SQ_CHEST,    0,    0,    GRP_NONE   },  // 33
  { "Bond Street",   SQ_PROPERTY,  320,   28,    GRP_GREEN  },  // 34
  { "Liverpool St. Station", SQ_RAILROAD, 200, 25, GRP_NONE },  // 35
  { "Chance",        SQ_CHANCE,      0,    0,    GRP_NONE   },  // 36
  { "Park Lane",     SQ_PROPERTY,  350,   35,    GRP_DBLUE  },  // 37
  { "Super Tax",     SQ_TAX,         0,  100,    GRP_NONE   },  // 38
  { "Mayfair",       SQ_PROPERTY,  400,   50,    GRP_DBLUE  },  // 39
};

// ── Chance / Community Chest cards ───────────────────────────
struct Card { const char* text; int delta; bool toJail; bool toGO; };

static const Card CHANCE[] = {
  { "Advance to GO! +$200",     200, false, true  },
  { "Bank dividend +$50",        50, false, false },
  { "Go to Jail",                 0, true,  false },
  { "Pay poor tax -$15",        -15, false, false },
  { "General repairs -$40",     -40, false, false },
};
static const Card CHEST[] = {
  { "Bank error! +$200",        200, false, false },
  { "Doctor fees -$50",         -50, false, false },
  { "Life insurance +$100",     100, false, false },
  { "School fees -$150",       -150, false, false },
  { "Services rendered +$25",    25, false, false },
};
#define CHANCE_N 5
#define CHEST_N  5
#define AUCTION_DURATION_MS 60000UL
#define BANK_LOAN_LIMIT 500

// ── Game State ───────────────────────────────────────────────
struct Player {
  char name[16];
  int  balance;
  int  position;
  bool inJail;
  bool connected;
  bool owns[NUM_SQ];   // which squares this player owns
};

enum Phase {
  PH_WAIT_PLAYERS,   // waiting for both to connect
  PH_LOBBY,          // both connected, 3s countdown
  PH_TURN_START,     // announce whose turn, send TURN msg
  PH_WAIT_ROLL,      // waiting for player to roll (touch or BLE)
  PH_DICE_ANIM,      // running dice animation (blocking)
  PH_WAIT_ACTION,    // square needs player decision (BUY/SKIP/OK) — wait for BLE from player card
  PH_ACTION_RESULT,  // show event result 3.5s then advance turn
  PH_AUCTION,        // auction running — 10s bid timer, auctioneer display
  PH_WAIT_TRADE,     // waiting for trade response from other player (30s timeout)
  PH_GAME_OVER,
  PH_CONFLICT,       // camera detected house/hotel mismatch — confirm or calibrate
  PH_CALIBRATE,      // camera coordinate calibration UI
  PH_CONFIRM_RESTART // mid-game restart confirmation screen
};

struct GameState {
  Player  P[NUM_PLY];
  int     sqOwner[NUM_SQ];   // -1, 0, or 1
  int     cur;               // current player index (0 or 1)
  Phase   phase;
  uint32_t phaseMs;          // millis() when phase was entered
  int     winner;            // -1, 0, or 1
  int     dice1, dice2;
  bool    lastRollWasDouble;  // current player gets another turn after resolving action
  int     doublesCount[NUM_PLY];
  char    evtMsg[96];        // event description for info panel
  bool    needRedraw;
  // Auction state
  int     auctionSq;         // square being auctioned (-1 = none)
  int     auctionBid;        // current highest bid (0 = no bids)
  int     auctionBidder;     // player index of highest bidder (-1 = none)
  uint32_t auctionTimerMs;   // millis() when auction timer last reset
  // Trade state
  int     tradeFrom;         // player index proposing trade
  int     tradeSqOffer;      // square being offered
  int     tradeSqWant;       // square being requested
  // Resend state: last ACT message that requires P1 response
  char    pendingActMsg[80]; // stored ACT:* message for resend
  uint32_t lastResendMs;     // millis() of last resend (or initial send)
  // House/hotel system records (camera integration)
  int     sysHouses[NUM_SQ];
  int     sysHotels[NUM_SQ];
  // Conflict / calibration state
  Phase   savedPhase;        // phase to return to after conflict/calibration
} G;

struct SaveData {
  uint32_t magic;
  uint16_t version;
  int      phase;
  int      savedPhase;
  Player   P[NUM_PLY];
  int      sqOwner[NUM_SQ];
  int      cur;
  int      winner;
  int      dice1, dice2;
  bool     lastRollWasDouble;
  int      doublesCount[NUM_PLY];
  char     evtMsg[96];
  int      auctionSq;
  int      auctionBid;
  int      auctionBidder;
  uint32_t auctionRemainingMs;
  int      tradeFrom;
  int      tradeSqOffer;
  int      tradeSqWant;
  char     pendingActMsg[80];
  int      sysHouses[NUM_SQ];
  int      sysHotels[NUM_SQ];
  bool     ignoredConflict[NUM_SQ];
  int      ignoredCamHouses[NUM_SQ], ignoredCamHotels[NUM_SQ];
  int      ignoredSysHouses[NUM_SQ], ignoredSysHotels[NUM_SQ];
};

static Preferences savePrefs;
static bool saveReady = false;
static const uint32_t SAVE_MAGIC = 0x4D4F4E4FUL;  // MONO
static const uint16_t SAVE_VERSION = 2;

// ── Camera ESP-NOW ──────────────────────────────────────────
// Camera (sender) MAC address — 替换为你的 camera 实际 MAC
static uint8_t cameraMAC[] = {0x58, 0x8C, 0x81, 0x9E, 0x62, 0xCC};
static volatile bool  cameraConnected = false;
static volatile bool  camDataReady    = false;
static char           camRxBuf[512]   = "";
static bool           espNowInited    = false;
static const uint8_t  ESPNOW_CHANNEL  = 1;

// Camera-detected board state (indexed by game square 0-39)
struct CameraCell { uint8_t houses; uint8_t hotels; };
static CameraCell camBoard[NUM_SQ];

// 多包重组缓冲
static int            camPktTotal     = 1;   // 预期总包数
static int            camPktRecv      = 0;   // 已收到包数
static CameraCell     camBuildBuf[41] = {};  // 重组中间缓冲

// Conflict list
struct Conflict {
  int  cellIdx;          // game square index 0-39
  int  camHouses, camHotels;
  int  sysHouses, sysHotels;
  char desc[80];
};
static Conflict conflicts[10];
static int conflictCount   = 0;
static int currentConflict = 0;

struct TouchRect {
  int x, y, w, h;
};

bool hitRect(int tx, int ty, const TouchRect& r) {
  return tx >= r.x && tx <= r.x + r.w && ty >= r.y && ty <= r.y + r.h;
}

// ── Restart button (drawn on gameplay screens) ──────────────
static const TouchRect rstBtn = { 720, 0, 80, 40 };

void drawRestartBtn() {
  display.fillRoundRect(rstBtn.x, rstBtn.y, rstBtn.w, rstBtn.h, 8, 0x880000UL);
  display.setTextColor(C_WHITE); display.setTextSize(2);
  int tw = display.textWidth("RST");
  display.setCursor(rstBtn.x + (rstBtn.w - tw) / 2, rstBtn.y + 10);
  display.print("RST");
}

static bool ignoredConflict[NUM_SQ];
static int  ignoredCamHouses[NUM_SQ], ignoredCamHotels[NUM_SQ];
static int  ignoredSysHouses[NUM_SQ], ignoredSysHotels[NUM_SQ];

void clearIgnoredConflict(int cellIdx) {
  if (cellIdx < 0 || cellIdx >= NUM_SQ) return;
  ignoredConflict[cellIdx] = false;
}

void ignoreCurrentConflict(const Conflict& c) {
  ignoredConflict[c.cellIdx] = true;
  ignoredCamHouses[c.cellIdx] = c.camHouses;
  ignoredCamHotels[c.cellIdx] = c.camHotels;
  ignoredSysHouses[c.cellIdx] = c.sysHouses;
  ignoredSysHotels[c.cellIdx] = c.sysHotels;
}

// ── Camera cell ID mapping (camera 1-40 → game 0-39) ────────
// Only the 22 buildable property squares
static const uint8_t CAM_ID_MAP[][2] = {
  { 2,  1}, { 4,  3}, { 7,  6}, { 9,  8}, {10,  9},
  {12, 11}, {14, 13}, {15, 14}, {17, 16}, {19, 18}, {20, 19},
  {22, 21}, {24, 23}, {25, 24}, {27, 26}, {28, 27}, {30, 29},
  {32, 31}, {33, 32}, {35, 34}, {38, 37}, {40, 39},
};
#define CAM_MAP_N (sizeof(CAM_ID_MAP) / sizeof(CAM_ID_MAP[0]))

int camIdToGameIdx(uint8_t camId) {
  for (int i = 0; i < (int)CAM_MAP_N; i++) {
    if (CAM_ID_MAP[i][0] == camId) return CAM_ID_MAP[i][1];
  }
  return -1;
}

void saveGame();
void clearSave();
bool isGameplayPhase(Phase p);

void enterPhase(Phase p) {
  G.phase        = p;
  G.phaseMs      = millis();
  G.lastResendMs = millis();  // reset heartbeat/resend timer on every phase change
  G.needRedraw   = true;
  if (p == PH_GAME_OVER) {
    clearSave();
  } else if (isGameplayPhase(p)) {
    saveGame();
  }
}
bool phaseElapsed(uint32_t ms) { return (millis() - G.phaseMs) >= ms; }

bool isGameplayPhase(Phase p) {
  return p != PH_WAIT_PLAYERS && p != PH_LOBBY && p != PH_GAME_OVER
      && p != PH_CONFIRM_RESTART;
}

int requiredBleConnections() {
#if SINGLE_DEVICE_TEST
  return 1;
#else
  return NUM_PLY;
#endif
}

static bool     blePauseActive  = false;
static uint32_t blePauseStartMs = 0;

// ── Single-device test mode ───────────────────────────────────
// Set to 1 to run with one physical player device acting as P1.
// P2 is auto-played by the central: auto-roll after 2s, auto-buy
// if affordable (balance > price), otherwise auto-skip.
#define SINGLE_DEVICE_TEST 0

// Temporary radio isolation test:
// 1 = do not start BLE, only keep WiFi/ESP-NOW alive for camera RX testing.
// Set back to 0 after confirming [CAM RX] appears.
#define CAMERA_ESPNOW_ONLY_TEST 0

// ── Skip BLE wait — dice / display test without player device ─
// Set to 1 to bypass BLE connection entirely and jump straight
// to the board + dice. Touch ROLL DICE (or wait 2s auto-roll).
// Set to 0 for real player card testing (BLE server is started).
#define SKIP_BLE_WAIT 0

// ── BLE Server ───────────────────────────────────────────────
#define BLE_NAME     "MonopolyCentral"
#define BLE_SVC_UUID "0000AA01-0000-1000-8000-00805F9B34FB"
#define BLE_NOTIFY   "0000BB01-0000-1000-8000-00805F9B34FB"
#define BLE_WRITE    "0000BB02-0000-1000-8000-00805F9B34FB"

static NimBLECharacteristic* pNotify   = nullptr;
static volatile int           bleConns  = 0;
static char                   bleRxBuf[72] = "";
static volatile bool          bleRxNew  = false;

void sendAll(const char* msg) {
  if (!pNotify || bleConns == 0) return;
  pNotify->setValue((uint8_t*)msg, strlen(msg));
  pNotify->notify();
  Serial0.printf("[TX] %s\n", msg);
}

void sendOwners();

void sendState() {
  char buf[64];
  snprintf(buf, sizeof(buf), "STATE:%d:%d:%d:%d",
    G.P[0].position, G.P[0].balance,
    G.P[1].position, G.P[1].balance);
  sendAll(buf);
  delay(25);
  sendOwners();
}

void sendOwners() {
  char buf[56];
  strcpy(buf, "OWNERS:");
  for (int i = 0; i < NUM_SQ; i++) {
    char c = '-';
    if (G.sqOwner[i] == 0) c = '1';
    else if (G.sqOwner[i] == 1) c = '2';
    buf[7 + i] = c;
  }
  buf[47] = '\0';
  sendAll(buf);
}

class BLESrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, ble_gap_conn_desc*) override {
    bleConns++;
    Serial0.printf("[BLE] Connected. Total=%d\n", bleConns);
    if (bleConns < 2) NimBLEDevice::startAdvertising();
    G.needRedraw = true;
  }
  void onDisconnect(NimBLEServer*) override {
    if (bleConns > 0) bleConns--;
    Serial0.printf("[BLE] Disconnected. Total=%d\n", bleConns);
    NimBLEDevice::startAdvertising();
    G.needRedraw = true;
  }
};

class BLEWrCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    auto v = c->getValue();
    snprintf(bleRxBuf, sizeof(bleRxBuf), "%.*s",
             (int)min((size_t)v.size(), (size_t)71), v.c_str());
    bleRxNew = true;
    Serial0.printf("[RX] %s\n", bleRxBuf);
  }
};

void startBLE() {
  NimBLEDevice::init(BLE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P3);
  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new BLESrvCB());
  NimBLEService* svc = srv->createService(BLE_SVC_UUID);
  pNotify = svc->createCharacteristic(BLE_NOTIFY, NIMBLE_PROPERTY::NOTIFY);
  auto* wr = svc->createCharacteristic(
    BLE_WRITE, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  wr->setCallbacks(new BLEWrCB());
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  adv->start();
  Serial0.println("[BLE] Server started: " BLE_NAME);
}

// ── Camera ESP-NOW (receive + send calibration) ─────────────
// Forward declarations
void parseCamJson(const char* json);

// 解析单包中的 cells 数组，累加到 camBuildBuf
static void parseCamPacket(const char* json) {
  const char* p = strstr(json, "\"cells\":");
  if (!p) return;
  p += 8;
  while (*p) {
    const char* obj = strchr(p, '{');
    if (!obj) break;
    const char* end = strchr(obj, '}');
    if (!end) break;
    int id = 0, houses = 0, hotels = 0;
    const char* idPos = strstr(obj, "\"id\":");
    if (idPos && idPos < end) id = atoi(idPos + 5);
    const char* hPos = strstr(obj, "\"houses\":");
    if (hPos && hPos < end) houses = atoi(hPos + 9);
    const char* htPos = strstr(obj, "\"hotels\":");
    if (htPos && htPos < end) hotels = atoi(htPos + 9);
    if (id > 0 && id <= 40) {
      camBuildBuf[id].houses = houses;
      camBuildBuf[id].hotels = hotels;
    }
    p = end + 1;
  }
}

// ESP-NOW 接收回调：camera 推送棋盘状态 JSON（支持分包）
// 单包格式: {"cells":[...]}  (旧协议，兼容)
// 多包格式: {"p":1,"t":3,"cells":[...]}
void camEspNowRecvCB(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len <= 0 || len >= (int)sizeof(camRxBuf)) return;
  memcpy(camRxBuf, data, len);
  camRxBuf[len] = '\0';

  // 收到 camera 数据就标记为已连接
  if (!cameraConnected) {
    cameraConnected = true;
    G.needRedraw = true;
  }
  Serial0.printf("[CAM RX] %s\n", camRxBuf);

  // 检测是否为分包协议
  const char* pField = strstr(camRxBuf, "\"p\":");
  const char* tField = strstr(camRxBuf, "\"t\":");
  if (pField && tField) {
    int pkt = atoi(pField + 4);
    int total = atoi(tField + 4);
    if (pkt == 1) {
      // 第一包：清空重组缓冲
      memset(camBuildBuf, 0, sizeof(camBuildBuf));
      camPktTotal = total;
      camPktRecv = 0;
    }
    parseCamPacket(camRxBuf);
    camPktRecv++;
    Serial0.printf("[CAM RX] packet %d/%d\n", pkt, total);
    if (camPktRecv >= camPktTotal) {
      // 所有包收齐，拷贝到 camRxBuf 触发处理
      // 直接把 camBuildBuf 内容组装成完整 JSON
      String full = "{\"cells\":[";
      bool first = true;
      for (int i = 1; i <= 40; i++) {
        if (camBuildBuf[i].houses > 0 || camBuildBuf[i].hotels > 0) {
          if (!first) full += ",";
          full += "{\"id\":";
          full += i;
          if (camBuildBuf[i].hotels > 0) { full += ",\"hotels\":"; full += camBuildBuf[i].hotels; }
          if (camBuildBuf[i].houses > 0) { full += ",\"houses\":"; full += camBuildBuf[i].houses; }
          full += "}";
          first = false;
        }
      }
      full += "]}";
      strncpy(camRxBuf, full.c_str(), sizeof(camRxBuf) - 1);
      camRxBuf[sizeof(camRxBuf) - 1] = '\0';
      camDataReady = true;
      Serial0.printf("[CAM RX] reassembled: %s\n", camRxBuf);
    }
  } else {
    // 旧协议：单包直接处理
    camDataReady = true;
  }
}

// ESP-NOW 发送回调
void camEspNowSendCB(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial0.println("[CAM] ESP-NOW send failed");
  }
}

void startCameraEspNow() {
  if (espNowInited) return;

  // 用 Arduino WiFi API（自动处理事件循环 + BLE 共存）
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);             // 禁用省电
  esp_err_t chErr = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (chErr != ESP_OK) Serial0.printf("[CAM] set channel failed: %d\n", chErr);

  // 读取 MAC
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial0.printf("[CAM] Central MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  if (esp_now_init() != ESP_OK) {
    Serial0.println("[CAM] ESP-NOW init FAILED!");
    return;
  }
  esp_now_register_recv_cb(camEspNowRecvCB);
  esp_now_register_send_cb(camEspNowSendCB);

  // 添加 camera 为 peer（用于发送校准指令）
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, cameraMAC, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;
  esp_err_t peerErr = esp_now_add_peer(&peerInfo);
  if (peerErr != ESP_OK && peerErr != ESP_ERR_ESPNOW_EXIST) {
    Serial0.printf("[CAM] add camera peer failed: %d\n", peerErr);
  }

  espNowInited = true;
  Serial0.println("[CAM] ESP-NOW ready, waiting for camera data...");
}

// Send calibration command to camera via ESP-NOW
void sendCamCalibration(const char* json) {
  if (!espNowInited) return;
  esp_now_send(cameraMAC, (uint8_t*)json, strlen(json));
  Serial0.printf("[CAM TX] %s\n", json);
}

// ── Game Logic ───────────────────────────────────────────────
Phase normalizeSavedPhase(Phase p, Phase savedPhase) {
  if (p == PH_DICE_ANIM) return PH_WAIT_ROLL;
  if (p == PH_CONFLICT || p == PH_CALIBRATE) {
    return isGameplayPhase(savedPhase) ? savedPhase : PH_WAIT_ROLL;
  }
  if (!isGameplayPhase(p)) return PH_WAIT_ROLL;
  return p;
}

void clearSave() {
  if (!saveReady) return;
  savePrefs.remove("game");
  Serial0.println("[SAVE] Cleared");
}

void saveGame() {
  if (!saveReady || !isGameplayPhase(G.phase)) return;

  SaveData s;
  memset(&s, 0, sizeof(s));
  s.magic = SAVE_MAGIC;
  s.version = SAVE_VERSION;
  Phase savedPhase = normalizeSavedPhase(G.phase, G.savedPhase);
  s.phase = (int)savedPhase;
  s.savedPhase = (int)normalizeSavedPhase(G.savedPhase, PH_WAIT_ROLL);

  memcpy(s.P, G.P, sizeof(s.P));
  for (int p = 0; p < NUM_PLY; p++) s.P[p].connected = false;
  memcpy(s.sqOwner, G.sqOwner, sizeof(s.sqOwner));
  s.cur = G.cur;
  s.winner = G.winner;
  s.dice1 = G.dice1;
  s.dice2 = G.dice2;
  s.lastRollWasDouble = G.lastRollWasDouble;
  memcpy(s.doublesCount, G.doublesCount, sizeof(s.doublesCount));
  strncpy(s.evtMsg, G.evtMsg, sizeof(s.evtMsg) - 1);
  s.auctionSq = G.auctionSq;
  s.auctionBid = G.auctionBid;
  s.auctionBidder = G.auctionBidder;

  s.auctionRemainingMs = AUCTION_DURATION_MS;
  if (G.phase == PH_AUCTION && G.auctionSq >= 0) {
    uint32_t elapsed = millis() - G.auctionTimerMs;
    s.auctionRemainingMs = elapsed >= AUCTION_DURATION_MS ? 0 : AUCTION_DURATION_MS - elapsed;
  }

  s.tradeFrom = G.tradeFrom;
  s.tradeSqOffer = G.tradeSqOffer;
  s.tradeSqWant = G.tradeSqWant;
  strncpy(s.pendingActMsg, G.pendingActMsg, sizeof(s.pendingActMsg) - 1);
  memcpy(s.sysHouses, G.sysHouses, sizeof(s.sysHouses));
  memcpy(s.sysHotels, G.sysHotels, sizeof(s.sysHotels));
  memcpy(s.ignoredConflict, ignoredConflict, sizeof(s.ignoredConflict));
  memcpy(s.ignoredCamHouses, ignoredCamHouses, sizeof(s.ignoredCamHouses));
  memcpy(s.ignoredCamHotels, ignoredCamHotels, sizeof(s.ignoredCamHotels));
  memcpy(s.ignoredSysHouses, ignoredSysHouses, sizeof(s.ignoredSysHouses));
  memcpy(s.ignoredSysHotels, ignoredSysHotels, sizeof(s.ignoredSysHotels));

  size_t written = savePrefs.putBytes("game", &s, sizeof(s));
  Serial0.printf("[SAVE] phase=%d bytes=%u\n", s.phase, (unsigned)written);
}

bool loadGame() {
  if (!saveReady) return false;
  if (savePrefs.getBytesLength("game") != sizeof(SaveData)) return false;

  SaveData s;
  size_t got = savePrefs.getBytes("game", &s, sizeof(s));
  if (got != sizeof(s) || s.magic != SAVE_MAGIC || s.version != SAVE_VERSION) {
    clearSave();
    return false;
  }

  Phase p = normalizeSavedPhase((Phase)s.phase, (Phase)s.savedPhase);
  memcpy(G.P, s.P, sizeof(G.P));
  for (int i = 0; i < NUM_PLY; i++) G.P[i].connected = false;
  memcpy(G.sqOwner, s.sqOwner, sizeof(G.sqOwner));
  G.cur = s.cur;
  G.phase = p;
  G.phaseMs = millis();
  G.winner = s.winner;
  G.dice1 = s.dice1;
  G.dice2 = s.dice2;
  G.lastRollWasDouble = s.lastRollWasDouble;
  memcpy(G.doublesCount, s.doublesCount, sizeof(G.doublesCount));
  strncpy(G.evtMsg, s.evtMsg, sizeof(G.evtMsg) - 1);
  G.evtMsg[sizeof(G.evtMsg) - 1] = '\0';
  G.needRedraw = true;
  G.auctionSq = s.auctionSq;
  G.auctionBid = s.auctionBid;
  G.auctionBidder = s.auctionBidder;
  uint32_t remaining = min(s.auctionRemainingMs, AUCTION_DURATION_MS);
  G.auctionTimerMs = millis() - (AUCTION_DURATION_MS - remaining);
  G.tradeFrom = s.tradeFrom;
  G.tradeSqOffer = s.tradeSqOffer;
  G.tradeSqWant = s.tradeSqWant;
  strncpy(G.pendingActMsg, s.pendingActMsg, sizeof(G.pendingActMsg) - 1);
  G.pendingActMsg[sizeof(G.pendingActMsg) - 1] = '\0';
  G.lastResendMs = millis();
  memcpy(G.sysHouses, s.sysHouses, sizeof(G.sysHouses));
  memcpy(G.sysHotels, s.sysHotels, sizeof(G.sysHotels));
  G.savedPhase = normalizeSavedPhase((Phase)s.savedPhase, PH_WAIT_ROLL);

  memcpy(ignoredConflict, s.ignoredConflict, sizeof(ignoredConflict));
  memcpy(ignoredCamHouses, s.ignoredCamHouses, sizeof(ignoredCamHouses));
  memcpy(ignoredCamHotels, s.ignoredCamHotels, sizeof(ignoredCamHotels));
  memcpy(ignoredSysHouses, s.ignoredSysHouses, sizeof(ignoredSysHouses));
  memcpy(ignoredSysHotels, s.ignoredSysHotels, sizeof(ignoredSysHotels));

  currentConflict = 0;
  conflictCount = 0;
  blePauseActive = false;
  Serial0.printf("[SAVE] Restored phase=%d cur=P%d\n", (int)G.phase, G.cur + 1);
  return true;
}

void initGame() {
  clearSave();
  for (int p = 0; p < NUM_PLY; p++) {
    G.P[p].balance  = START_BAL;
    G.P[p].position = 0;
    G.P[p].inJail   = false;
    memset(G.P[p].owns, 0, sizeof(G.P[p].owns));
  }
  for (int i = 0; i < NUM_SQ; i++) G.sqOwner[i] = -1;
  G.cur    = 0;
  G.winner = -1;
  G.dice1  = G.dice2 = 0;
  G.lastRollWasDouble = false;
  for (int p = 0; p < NUM_PLY; p++) G.doublesCount[p] = 0;
  G.evtMsg[0] = '\0';
  memset(G.sysHouses, 0, sizeof(G.sysHouses));
  memset(G.sysHotels, 0, sizeof(G.sysHotels));
  memset(ignoredConflict, 0, sizeof(ignoredConflict));
}

// ── Camera JSON Parser ──────────────────────────────────────
// Parse {"cells":[{"id":5,"hotels":1},{"id":12,"houses":2}]}
void parseCamJson(const char* json) {
  memset(camBoard, 0, sizeof(camBoard));

  const char* p = strstr(json, "\"cells\":");
  if (!p) return;
  p += 8;

  // Walk through array of objects
  while (*p) {
    // Find next object
    const char* obj = strchr(p, '{');
    if (!obj) break;
    p = obj + 1;

    // Extract id, houses, hotels from this object
    int id = 0, houses = 0, hotels = 0;

    const char* idPos = strstr(obj, "\"id\":");
    if (idPos && idPos < strchr(obj, '}')) {
      id = atoi(idPos + 5);
    }
    const char* hPos = strstr(obj, "\"houses\":");
    if (hPos && hPos < strchr(obj, '}')) {
      houses = atoi(hPos + 9);
    }
    const char* htPos = strstr(obj, "\"hotels\":");
    if (htPos && htPos < strchr(obj, '}')) {
      hotels = atoi(htPos + 9);
    }

    if (id > 0) {
      int gIdx = camIdToGameIdx(id);
      if (gIdx >= 0 && gIdx < NUM_SQ) {
        camBoard[gIdx].houses = houses;
        camBoard[gIdx].hotels = hotels;
      }
    }

    // Move past closing brace
    const char* end = strchr(p, '}');
    if (!end) break;
    p = end + 1;
  }
}

// ── Conflict Detection ──────────────────────────────────────
void detectConflicts() {
  conflictCount = 0;
  currentConflict = 0;

  for (int i = 0; i < (int)CAM_MAP_N && conflictCount < 10; i++) {
    int gIdx = CAM_ID_MAP[i][1];
    int camH  = camBoard[gIdx].houses;
    int camHt = camBoard[gIdx].hotels;
    int sysH  = G.sysHouses[gIdx];
    int sysHt = G.sysHotels[gIdx];

    if (camH != sysH || camHt != sysHt) {
      if (ignoredConflict[gIdx] &&
          ignoredCamHouses[gIdx] == camH &&
          ignoredCamHotels[gIdx] == camHt &&
          ignoredSysHouses[gIdx] == sysH &&
          ignoredSysHotels[gIdx] == sysHt) {
        continue;
      }
      ignoredConflict[gIdx] = false;

      Conflict& c = conflicts[conflictCount];
      c.cellIdx   = gIdx;
      c.camHouses = camH;  c.camHotels = camHt;
      c.sysHouses = sysH;  c.sysHotels = sysHt;

      // Build description
      const char* name = SQ[gIdx].name;
      if (camH > sysH) {
        snprintf(c.desc, sizeof(c.desc), "%s: +%d house detected", name, camH - sysH);
      } else if (camH < sysH) {
        snprintf(c.desc, sizeof(c.desc), "%s: -%d house missing", name, sysH - camH);
      } else if (camHt > sysHt) {
        snprintf(c.desc, sizeof(c.desc), "%s: +%d hotel detected", name, camHt - sysHt);
      } else {
        snprintf(c.desc, sizeof(c.desc), "%s: -%d hotel missing", name, sysHt - camHt);
      }
      conflictCount++;
    } else {
      clearIgnoredConflict(gIdx);
    }
  }
}

// Move current player n steps; returns true if passed GO
bool movePlayer(int steps) {
  int& pos   = G.P[G.cur].position;
  int  oldPos = pos;
  pos = (pos + steps) % NUM_SQ;
  bool passed = (pos < oldPos);     // wrapped around = passed GO
  if (passed) {
    G.P[G.cur].balance += GO_BONUS;
    Serial0.printf("[GAME] P%d passed GO +$%d\n", G.cur+1, GO_BONUS);
  }
  return passed;
}

bool ownsAllInGroup(int owner, uint32_t grp) {
  if (grp == GRP_NONE) return false;
  bool found = false;
  for (int i = 0; i < NUM_SQ; i++) {
    if (SQ[i].type == SQ_PROPERTY && SQ[i].grp == grp) {
      found = true;
      if (G.sqOwner[i] != owner) return false;
    }
  }
  return found;
}

int countOwnedType(int owner, SqType type) {
  int n = 0;
  for (int i = 0; i < NUM_SQ; i++) {
    if (SQ[i].type == type && G.sqOwner[i] == owner) n++;
  }
  return n;
}

int calcRent(int sq, int owner) {
  switch (SQ[sq].type) {
    case SQ_RAILROAD: {
      int n = countOwnedType(owner, SQ_RAILROAD);
      if (n <= 1) return 25;
      if (n == 2) return 50;
      if (n == 3) return 100;
      return 200;
    }
    case SQ_UTILITY: {
      int n = countOwnedType(owner, SQ_UTILITY);
      int dice = G.dice1 + G.dice2;
      return dice * (n >= 2 ? 10 : 4);
    }
    case SQ_PROPERTY: {
      int rent = SQ[sq].value;
      if (ownsAllInGroup(owner, SQ[sq].grp)) rent *= 2;
      return rent;
    }
    default:
      return SQ[sq].value;
  }
}

int playerNetWorth(int pIdx) {
  int worth = G.P[pIdx].balance;
  for (int i = 0; i < NUM_SQ; i++) {
    if (G.sqOwner[i] == pIdx) worth += SQ[i].price;
  }
  return worth;
}

int calcTaxDue(int pIdx, int sq) {
  if (strcmp(SQ[sq].name, "Income Tax") == 0) {
    int tenPercent = playerNetWorth(pIdx) / 10;
    return min(200, tenPercent);
  }
  return SQ[sq].value;
}

void declareBankrupt(int pIdx) {
  G.winner = 1 - pIdx;
  snprintf(G.evtMsg, sizeof(G.evtMsg),
    "%s BANKRUPT! %s WINS!", G.P[pIdx].name, G.P[G.winner].name);
  char buf[16];
  snprintf(buf, sizeof(buf), "BANKRUPT:%d", pIdx+1);
  sendAll(buf);
  delay(500);
  snprintf(buf, sizeof(buf), "WIN:%d", G.winner+1);
  sendAll(buf);
  enterPhase(PH_GAME_OVER);
}

void checkBankrupt() {
  for (int i = 0; i < NUM_PLY; i++) {
    if (G.P[i].balance < -BANK_LOAN_LIMIT) { declareBankrupt(i); return; }
  }
}

// Finish the current turn: send state, check bankrupt, schedule next turn
void endTurn() {
  sendState();
  delay(40);
  sendAll("DONE");
  checkBankrupt();
  if (G.phase == PH_GAME_OVER) return;
  enterPhase(PH_ACTION_RESULT);
}

void advanceAfterResult() {
  bool extraTurn = G.lastRollWasDouble && !G.P[G.cur].inJail;
  if (!extraTurn) G.cur = 1 - G.cur;
  G.lastRollWasDouble = false;
  G.dice1 = G.dice2 = 0;
  G.evtMsg[0] = '\0';
  enterPhase(PH_TURN_START);
}

// Forward declaration — defined in display section below
void drawCardScreen(bool isChance, const char* text, int delta);

// Called after dice roll — evaluate square and act
void processSquare(int pIdx, int sq) {
  Player& cur   = G.P[pIdx];
  Player& other = G.P[1 - pIdx];
  char    buf[80];

  switch (SQ[sq].type) {

    case SQ_GO:
      snprintf(G.evtMsg, sizeof(G.evtMsg),
        "P%d landed on GO! Collect $%d", pIdx+1, GO_BONUS);
      G.pendingActMsg[0] = '\0';  // auto-advances
      sendAll("ACT:GO:200");
      enterPhase(PH_WAIT_ACTION);   // player sees landing screen, taps OK
      break;

    case SQ_PROPERTY:
    case SQ_RAILROAD:
    case SQ_UTILITY: {
      int ow = G.sqOwner[sq];
      if (ow == -1) {
        // Unowned — offer to buy
        snprintf(G.evtMsg, sizeof(G.evtMsg),
          "P%d: Buy %s for $%d?", pIdx+1, SQ[sq].name, SQ[sq].price);
        snprintf(buf, sizeof(buf), "ACT:BUY:%s:%d", SQ[sq].name, SQ[sq].price);
        strncpy(G.pendingActMsg, buf, 79); G.lastResendMs = millis();
        sendAll(buf);
        enterPhase(PH_WAIT_ACTION);
      } else if (ow == pIdx) {
        snprintf(G.evtMsg, sizeof(G.evtMsg),
          "P%d: Own %s", pIdx+1, SQ[sq].name);
        sendAll("ACT:OWN");
        G.pendingActMsg[0] = '\0';  // auto-advances; no resend needed
        enterPhase(PH_WAIT_ACTION);  // show "YOUR PROPERTY", tap OK
      } else {
        int rent = calcRent(sq, ow);
        cur.balance   -= rent;
        other.balance += rent;
        snprintf(G.evtMsg, sizeof(G.evtMsg),
          "P%d pays $%d rent to P%d for %s",
          pIdx+1, rent, (1-pIdx)+1, SQ[sq].name);
        snprintf(buf, sizeof(buf), "ACT:RENT:%d:%s",
          rent, other.name);
        strncpy(G.pendingActMsg, buf, 79); G.lastResendMs = millis();
        sendAll(buf);
        enterPhase(PH_WAIT_ACTION);  // show "PAY RENT", tap PAY to confirm
      }
      break;
    }

    case SQ_TAX:
      {
      int tax = calcTaxDue(pIdx, sq);
      cur.balance -= tax;
      snprintf(G.evtMsg, sizeof(G.evtMsg),
        "P%d pays %s $%d", pIdx+1, SQ[sq].name, tax);
      snprintf(buf, sizeof(buf), "ACT:TAX:%d", tax);
      strncpy(G.pendingActMsg, buf, 79); G.lastResendMs = millis();
      sendAll(buf);
      enterPhase(PH_WAIT_ACTION);  // show tax screen, tap PAY to confirm
      }
      break;

    case SQ_CHANCE: {
      int idx = random(0, CHANCE_N);
      const Card& c = CHANCE[idx];
      snprintf(G.evtMsg, sizeof(G.evtMsg), "CHANCE: %s", c.text);
      snprintf(buf, sizeof(buf), "ACT:CHANCE:%s", c.text);
      sendAll(buf);
      if (c.toJail) {
        cur.position = JAIL_SQ;
        cur.inJail   = true;
        G.doublesCount[pIdx] = 0;
        G.lastRollWasDouble = false;
      } else if (c.toGO) {
        cur.position = 0;
        cur.balance += (c.delta != 0) ? c.delta : GO_BONUS;
      } else {
        cur.balance += c.delta;
      }
      drawCardScreen(true, c.text, c.delta);  // blocking card display
      endTurn();
      break;
    }

    case SQ_CHEST: {
      int idx = random(0, CHEST_N);
      const Card& c = CHEST[idx];
      snprintf(G.evtMsg, sizeof(G.evtMsg), "CHEST: %s", c.text);
      snprintf(buf, sizeof(buf), "ACT:CHEST:%s", c.text);
      sendAll(buf);
      cur.balance += c.delta;
      drawCardScreen(false, c.text, c.delta);  // blocking card display
      endTurn();
      break;
    }

    case SQ_JAIL:
      snprintf(G.evtMsg, sizeof(G.evtMsg), "P%d: Just Visiting Jail", pIdx+1);
      G.pendingActMsg[0] = '\0';  // auto-advances
      sendAll("ACT:VISIT");
      enterPhase(PH_WAIT_ACTION);  // show jail screen, tap OK
      break;

    case SQ_GOTO_JAIL:
      cur.position = JAIL_SQ;
      cur.inJail   = true;
      G.doublesCount[pIdx] = 0;
      G.lastRollWasDouble = false;
      snprintf(G.evtMsg, sizeof(G.evtMsg), "P%d: Go to Jail!", pIdx+1);
      G.pendingActMsg[0] = '\0';  // auto-advances
      sendAll("ACT:JAIL");
      enterPhase(PH_WAIT_ACTION);  // show jail screen, tap OK
      break;

    case SQ_FREE_PARK:
      snprintf(G.evtMsg, sizeof(G.evtMsg), "P%d: Free Parking", pIdx+1);
      G.pendingActMsg[0] = '\0';  // auto-advances
      sendAll("ACT:FREE");
      enterPhase(PH_WAIT_ACTION);  // show free parking screen, tap OK
      break;
  }
}

void resyncCurrentState() {
  if (!isGameplayPhase(G.phase)) return;

  char buf[64];
  snprintf(buf, sizeof(buf), "TURN:%d", G.cur + 1);
  sendAll(buf);
  delay(40);
  sendState();

  if (G.phase == PH_WAIT_ACTION && G.pendingActMsg[0]) {
    delay(40);
    sendAll(G.pendingActMsg);
  } else if (G.phase == PH_AUCTION && G.auctionSq >= 0) {
    delay(40);
    snprintf(buf, sizeof(buf), "AUCTION:%d:%d", G.auctionSq, SQ[G.auctionSq].price);
    sendAll(buf);
    if (G.auctionBidder >= 0) {
      delay(40);
      snprintf(buf, sizeof(buf), "BID:%d:%d", G.auctionBidder + 1, G.auctionBid);
      sendAll(buf);
    }
  } else if (G.phase == PH_WAIT_TRADE) {
    delay(40);
    snprintf(buf, sizeof(buf), "TRADE:%d:%d:%d",
      G.tradeFrom + 1, G.tradeSqOffer, G.tradeSqWant);
    sendAll(buf);
  } else if (G.phase == PH_ACTION_RESULT) {
    delay(40);
    sendAll("DONE");
  }
}

void drawBlePausedOverlay() {
  display.fillRoundRect(120, 150, 560, 170, 12, 0x18183AUL);
  display.drawRoundRect(120, 150, 560, 170, 12, C_RED);

  display.setTextColor(C_RED); display.setTextSize(3);
  int tw = display.textWidth("BLE DISCONNECTED");
  display.setCursor((800 - tw) / 2, 180);
  display.print("BLE DISCONNECTED");

  display.setTextColor(C_WHITE); display.setTextSize(2);
  tw = display.textWidth("Game paused. Reconnect player cards.");
  display.setCursor((800 - tw) / 2, 235);
  display.print("Game paused. Reconnect player cards.");

  display.setTextColor(C_GRAY); display.setTextSize(1);
  char line[48];
  snprintf(line, sizeof(line), "Connected BLE devices: %d / %d", bleConns, requiredBleConnections());
  tw = display.textWidth(line);
  display.setCursor((800 - tw) / 2, 285);
  display.print(line);

  flushDisp();
}

void updateBlePause() {
  bool shouldPause = isGameplayPhase(G.phase) && bleConns < requiredBleConnections();

  if (shouldPause && !blePauseActive) {
    blePauseActive = true;
    blePauseStartMs = millis();
    G.needRedraw = true;
    Serial0.println("[BLE] Game paused until player cards reconnect");
  } else if (!shouldPause && blePauseActive) {
    uint32_t pausedMs = millis() - blePauseStartMs;
    G.phaseMs += pausedMs;
    G.lastResendMs += pausedMs;
    if (G.phase == PH_AUCTION) G.auctionTimerMs += pausedMs;
    blePauseActive = false;
    G.needRedraw = true;
    delay(200);
    resyncCurrentState();
    Serial0.printf("[BLE] Game resumed after %lu ms\n", (unsigned long)pausedMs);
  }
}

void finishAuction() {
  if (G.auctionSq < 0) return;

  if (G.auctionBidder >= 0) {
    G.sqOwner[G.auctionSq]                 = G.auctionBidder;
    G.P[G.auctionBidder].owns[G.auctionSq] = true;
    G.P[G.auctionBidder].balance          -= G.auctionBid;
    snprintf(G.evtMsg, sizeof(G.evtMsg),
      "P%d won auction: %s for $%d",
      G.auctionBidder + 1, SQ[G.auctionSq].name, G.auctionBid);
    char buf[48];
    snprintf(buf, sizeof(buf), "AUCTION:WIN:%d:%d:%d",
      G.auctionBidder + 1, G.auctionSq, G.auctionBid);
    sendAll(buf);
  } else {
    snprintf(G.evtMsg, sizeof(G.evtMsg),
      "Auction: no bids for %s", SQ[G.auctionSq].name);
    sendAll("AUCTION:NOBID");
  }

  G.auctionSq = -1;
  endTurn();
}

// Process incoming BLE message from player device
void handleBLERx(const char* msg) {
  // Parse player index from "P1:" or "P2:" prefix
  int pIdx = -1;
  if (msg[0]=='P' && msg[2]==':') {
    if (msg[1]=='1') pIdx = 0;
    else if (msg[1]=='2') pIdx = 1;
  }
  if (pIdx < 0) return;
  const char* cmd = msg + 3;

  // Registration (any phase)
  if (strcmp(cmd, "HELLO") == 0) {
    G.P[pIdx].connected = true;
    Serial0.printf("[GAME] P%d registered\n", pIdx+1);
    G.needRedraw = true;
    // If game already in progress, resync immediately.
    // Player may have missed TURN/ACT notifications (BLE drop or reconnect).
    if (isGameplayPhase(G.phase)) {
      delay(200);  // let BLE subscription settle before sending
      resyncCurrentState();
      Serial0.printf("[RESYNC] Sent state to P%d (phase=%d)\n", pIdx+1, G.phase);
    }
    return;
  }

  if (!G.P[0].connected || !G.P[1].connected) return;

  // Roll request — only from the current player, only in WAIT_ROLL
  if (strcmp(cmd, "ROLL") == 0 &&
      pIdx == G.cur &&
      G.phase == PH_WAIT_ROLL) {
    enterPhase(PH_DICE_ANIM);
    return;
  }

  // ── Auction bids (any phase = PH_AUCTION, any player) ───────
  if (G.phase == PH_AUCTION) {
    // P{n}:BID:{amt}
    if (strncmp(cmd, "BID:", 4) == 0) {
      int amt = atoi(cmd + 4);
      if (amt > G.auctionBid && amt > 0) {
        G.auctionBid    = amt;
        G.auctionBidder = pIdx;
        G.auctionTimerMs = millis();   // reset 10s timer on valid bid
        char buf[32];
        snprintf(buf, sizeof(buf), "BID:%d:%d", pIdx+1, amt);
        sendAll(buf);
        Serial0.printf("[AUC] P%d bid $%d\n", pIdx+1, amt);
        G.needRedraw = true;
        saveGame();
      }
    }
    // P{n}:PASS — no action needed; just ignore (timer still running)
    return;
  }

  // ── Trade responses (PH_WAIT_TRADE, other player only) ───────
  if (G.phase == PH_WAIT_TRADE && pIdx != G.tradeFrom) {
    if (strcmp(cmd, "TRADE:YES") == 0) {
      // Execute swap
      int a = G.tradeSqOffer, b = G.tradeSqWant;
      G.sqOwner[a] = 1 - G.tradeFrom;
      G.P[1 - G.tradeFrom].owns[a] = true;  G.P[G.tradeFrom].owns[a] = false;
      G.sqOwner[b] = G.tradeFrom;
      G.P[G.tradeFrom].owns[b] = true;       G.P[1 - G.tradeFrom].owns[b] = false;
      snprintf(G.evtMsg, sizeof(G.evtMsg),
        "Trade: P%d got %s / P%d got %s",
        G.tradeFrom+1, SQ[b].name, (1-G.tradeFrom)+1, SQ[a].name);
      sendAll("TRADE:DONE");
      endTurn();
    } else if (strcmp(cmd, "TRADE:NO") == 0) {
      snprintf(G.evtMsg, sizeof(G.evtMsg), "Trade rejected");
      sendAll("TRADE:FAIL");
      endTurn();
    }
    return;
  }

  // ── Action responses — only from current player in WAIT_ACTION ─
  if (G.phase != PH_WAIT_ACTION || pIdx != G.cur) return;
  int sq = G.P[pIdx].position;

  G.pendingActMsg[0] = '\0';  // stop resend — player responded

  if (strcmp(cmd, "BUY") == 0) {
    G.P[pIdx].balance -= SQ[sq].price;
    G.sqOwner[sq]      = pIdx;
    G.P[pIdx].owns[sq] = true;
    snprintf(G.evtMsg, sizeof(G.evtMsg),
      "P%d bought %s for $%d",
      pIdx+1, SQ[sq].name, SQ[sq].price);
    endTurn();

  } else if (strcmp(cmd, "SKIP") == 0) {
    // Decline to buy — start auction
    G.auctionSq      = sq;
    G.auctionBid     = 0;
    G.auctionBidder  = -1;
    G.auctionTimerMs = millis();
    char buf[32];
    snprintf(buf, sizeof(buf), "AUCTION:%d:%d", sq, SQ[sq].price);
    sendAll(buf);
    Serial0.printf("[AUC] Started for sq%d %s at $%d\n", sq, SQ[sq].name, SQ[sq].price);
    enterPhase(PH_AUCTION);

  } else if (strcmp(cmd, "OK") == 0) {
    snprintf(G.evtMsg, sizeof(G.evtMsg),
      "P%d confirmed %s", pIdx+1, SQ[sq].name);
    endTurn();

  } else if (strncmp(cmd, "TRADE:", 6) == 0) {
    // P{n}:TRADE:{sq_mine}:{sq_theirs}
    int sqMine, sqTheirs;
    if (sscanf(cmd + 6, "%d:%d", &sqMine, &sqTheirs) == 2) {
      // Validate: pIdx owns sqMine, other player owns sqTheirs
      if (G.sqOwner[sqMine] == pIdx && G.sqOwner[sqTheirs] == (1 - pIdx)) {
        G.tradeFrom    = pIdx;
        G.tradeSqOffer = sqMine;
        G.tradeSqWant  = sqTheirs;
        char buf[48];
        snprintf(buf, sizeof(buf), "TRADE:%d:%d:%d", pIdx+1, sqMine, sqTheirs);
        sendAll(buf);
        enterPhase(PH_WAIT_TRADE);
      }
    }
  }
}

// ── Dice Sprite (same technique as test: 2-die canvas → pushSprite) ─
#define DICE_SZ  155
#define DICE_GAP  50
#define DICE_SW  (DICE_SZ*2 + DICE_GAP)
static lgfx::LGFX_Sprite diceSprite;

void drawDieFace(lgfx::LovyanGFX* gfx, int x, int y, int sz,
                 int face, uint32_t bg, uint32_t pip) {
  gfx->fillRoundRect(x, y, sz, sz, sz/8, bg);
  gfx->drawRoundRect(x, y, sz, sz, sz/8, C_LGRAY);
  struct Pip { int c, r; };
  const Pip lay[6][6] = {
    {{1,1}},
    {{0,0},{2,2}},
    {{0,0},{1,1},{2,2}},
    {{0,0},{2,0},{0,2},{2,2}},
    {{0,0},{2,0},{1,1},{0,2},{2,2}},
    {{0,0},{2,0},{0,1},{2,1},{0,2},{2,2}},
  };
  const int cnt[6] = {1,2,3,4,5,6};
  int r = sz/10, m = sz/5, s = (sz - m*2) / 2;
  for (int i = 0; i < cnt[face-1]; i++) {
    gfx->fillCircle(x+m+lay[face-1][i].c*s,
                    y+m+lay[face-1][i].r*s, r, pip);
  }
}

void runDiceAnim() {
  bool useSprite = (diceSprite.width() == DICE_SW);
  const int TX = (800 - DICE_SW) / 2;
  const int TY = (480 - DICE_SZ) / 2;

  beep(40);

  // Draw static background ONCE — never touch it again during the loop
  display.fillScreen(C_BG);
  display.setTextColor(C_WHITE); display.setTextSize(3);
  int tw = display.textWidth("Tap to stop!");
  display.setCursor((800-tw)/2, 55);
  display.print("Tap to stop!");
  flushDisp();

  // Animation loop: runs until player taps the screen
  // Current face values become the actual dice result
  int curD1 = 1, curD2 = 1;
  bool wasTouched = true;  // start true to ignore lingering touch from ROLL button
  while (true) {
    curD1 = random(1, 7);
    curD2 = random(1, 7);
    if (useSprite) {
      diceSprite.fillScreen(C_BG);
      drawDieFace(&diceSprite, 0,              0, DICE_SZ, curD1, C_WHITE, C_BLACK);
      drawDieFace(&diceSprite, DICE_SZ+DICE_GAP, 0, DICE_SZ, curD2, C_WHITE, C_BLACK);
      diceSprite.pushSprite(&display, TX, TY);
    } else {
      display.fillRect(TX, TY, DICE_SW, DICE_SZ, C_BG);
      drawDieFace(&display, TX,              TY, DICE_SZ, curD1, C_WHITE, C_BLACK);
      drawDieFace(&display, TX+DICE_SZ+DICE_GAP, TY, DICE_SZ, curD2, C_WHITE, C_BLACK);
    }
    flushDisp();

    // Check for touch (new press only — must release first)
    lgfx::touch_point_t tp[1];
    bool touching = display.getTouch(tp, 1) > 0;
    if (touching && !wasTouched) break;  // tap detected → stop
    wasTouched = touching;

    delay(80);  // ~12.5 fps — fast enough to feel random
  }

  // The displayed faces are the result
  G.dice1 = curD1;
  G.dice2 = curD2;
  bool rolledDouble = (G.dice1 == G.dice2);
  if (rolledDouble) G.doublesCount[G.cur]++;
  else G.doublesCount[G.cur] = 0;

  bool thirdDouble = (G.doublesCount[G.cur] >= 3);
  bool passedGO = false;
  if (thirdDouble) {
    G.P[G.cur].position = JAIL_SQ;
    G.P[G.cur].inJail = true;
    G.doublesCount[G.cur] = 0;
    G.lastRollWasDouble = false;
  } else {
    G.lastRollWasDouble = rolledDouble;
    passedGO = movePlayer(G.dice1 + G.dice2);
  }
  int  sq       = G.P[G.cur].position;

  display.fillScreen(C_BG);

  if (useSprite) {
    diceSprite.fillScreen(C_BG);
    drawDieFace(&diceSprite, 0,              0, DICE_SZ, G.dice1, C_GOLD, C_BLACK);
    drawDieFace(&diceSprite, DICE_SZ+DICE_GAP, 0, DICE_SZ, G.dice2, C_GOLD, C_BLACK);
    diceSprite.pushSprite(&display, TX, TY);
  } else {
    drawDieFace(&display, TX,              TY, DICE_SZ, G.dice1, C_GOLD, C_BLACK);
    drawDieFace(&display, TX+DICE_SZ+DICE_GAP, TY, DICE_SZ, G.dice2, C_GOLD, C_BLACK);
  }

  display.setTextColor(C_WHITE); display.setTextSize(3);
  char res[48];
  if (thirdDouble) {
    snprintf(res, sizeof(res), "3 doubles -> Jail");
  } else {
    snprintf(res, sizeof(res), "Rolled %d -> %s", G.dice1+G.dice2, SQ[sq].name);
  }
  tw = display.textWidth(res);
  display.setCursor((800-tw)/2, TY + DICE_SZ + 18);
  display.print(res);

  if (passedGO) {
    display.setTextColor(C_GREEN); display.setTextSize(2);
    tw = display.textWidth("Passed GO! +$200");
    display.setCursor((800-tw)/2, TY + DICE_SZ + 54);
    display.print("Passed GO! +$200");
  }

  if (rolledDouble) {
    display.setTextColor(C_ORANGE); display.setTextSize(3);
    const char* dblTxt = thirdDouble ? "THIRD DOUBLE!" : "DOUBLES!";
    tw = display.textWidth(dblTxt);
    display.setCursor((800-tw)/2, TY + DICE_SZ + (passedGO ? 84 : 54));
    display.print(dblTxt);
    beep(200);
  } else {
    beep(80);
  }

  display.setTextColor(C_GRAY); display.setTextSize(2);
  tw = display.textWidth("Tap screen to continue");
  display.setCursor((800-tw)/2, 446);
  display.print("Tap screen to continue");
  drawRestartBtn();
  flushDisp();

  // Broadcast dice result and new position
  char msg[32];
  snprintf(msg, sizeof(msg), "DICE:%d", G.dice1+G.dice2);
  sendAll(msg);
  delay(40);
  sendState();

  // Wait for touch before processing the square
  {
    bool wasT = false;
    while (true) {
      lgfx::touch_point_t tp2[1];
      bool t2 = display.getTouch(tp2, 1) > 0;
      if (t2 && !wasT) break;
      wasT = t2;
      delay(16);
    }
  }
  if (thirdDouble) {
    snprintf(G.evtMsg, sizeof(G.evtMsg),
      "P%d rolled doubles three times: Go to Jail!", G.cur+1);
    G.pendingActMsg[0] = '\0';
    sendAll("ACT:JAIL");
    enterPhase(PH_WAIT_ACTION);
  } else {
    processSquare(G.cur, sq);
  }
}

// ── Turn Screen (PH_TURN_START and PH_WAIT_ROLL) ─────────────
// showRoll=true adds the centered ROLL DICE button
void drawTurnScreen(int cur, bool showRoll) {
  display.fillScreen((cur == 0) ? 0x3A0808UL : 0x08103AUL);

  // Header bar in player color
  display.fillRect(0, 0, 800, 100, P_COLOR[cur]);
  display.setTextColor(C_WHITE); display.setTextSize(5);
  char hdr[28]; snprintf(hdr, sizeof(hdr), "PLAYER %d'S TURN", cur+1);
  int tw = display.textWidth(hdr);
  display.setCursor((800-tw)/2, 22);
  display.print(hdr);

  // Info card
  display.fillRoundRect(150, 120, 500, 210, 20, 0x0C0C28UL);
  display.drawRoundRect(150, 120, 500, 210, 20, P_COLOR[cur]);

  display.setTextColor(P_COLOR[cur]); display.setTextSize(3);
  tw = display.textWidth(G.P[cur].name);
  display.setCursor((800-tw)/2, 140);
  display.print(G.P[cur].name);

  display.setTextColor(C_GREEN); display.setTextSize(2);
  char bal[24]; snprintf(bal, sizeof(bal), "Balance: $%d", G.P[cur].balance);
  tw = display.textWidth(bal);
  display.setCursor((800-tw)/2, 190);
  display.print(bal);

  display.setTextColor(C_LGRAY); display.setTextSize(2);
  char pos[40]; snprintf(pos, sizeof(pos), "Position: %s", SQ[G.P[cur].position].name);
  tw = display.textWidth(pos);
  display.setCursor((800-tw)/2, 226);
  display.print(pos);

  if (G.P[cur].inJail) {
    display.setTextColor(C_RED); display.setTextSize(2);
    tw = display.textWidth("IN JAIL — bail paid at turn start");
    display.setCursor((800-tw)/2, 262);
    display.print("IN JAIL — bail paid at turn start");
  }

  if (showRoll) {
    // Large centered ROLL DICE button
    display.fillRoundRect(200, 358, 400, 82, 16, C_BLUE);
    display.drawRoundRect(200, 358, 400, 82, 16, C_WHITE);
    display.setTextColor(C_WHITE); display.setTextSize(3);
    tw = display.textWidth("ROLL DICE");
    display.setCursor((800-tw)/2, 383);
    display.print("ROLL DICE");

  } else {
    display.setTextColor(C_LGRAY); display.setTextSize(2);
    tw = display.textWidth("Tap screen to continue");
    display.setCursor((800-tw)/2, 390);
    display.print("Tap screen to continue");
  }

  drawRestartBtn();
  flushDisp();
}

// ── Square Landing Screen (PH_WAIT_ACTION) ───────────────────
void drawSquareLanding(int pIdx, int sq) {
  display.fillScreen(C_BG);

  // Top color bar for property group
  if (SQ[sq].grp != GRP_NONE)
    display.fillRect(0, 0, 800, 28, SQ[sq].grp);

  // Square name (large)
  display.setTextColor(C_WHITE); display.setTextSize(5);
  int tw = display.textWidth(SQ[sq].name);
  display.setCursor((800-tw)/2, 40);
  display.print(SQ[sq].name);

  // Type label
  const char* typeLabel = "";
  switch (SQ[sq].type) {
    case SQ_PROPERTY:  typeLabel = "PROPERTY";        break;
    case SQ_RAILROAD:  typeLabel = "RAILROAD";        break;
    case SQ_UTILITY:   typeLabel = "UTILITY";         break;
    case SQ_TAX:       typeLabel = "TAX";             break;
    case SQ_GO:        typeLabel = "GO";              break;
    case SQ_CHANCE:    typeLabel = "CHANCE";          break;
    case SQ_CHEST:     typeLabel = "COMMUNITY CHEST"; break;
    case SQ_JAIL:      typeLabel = "JAIL";            break;
    case SQ_GOTO_JAIL: typeLabel = "GO TO JAIL";      break;
    case SQ_FREE_PARK: typeLabel = "FREE PARKING";    break;
    default: break;
  }
  display.setTextColor(C_LGRAY); display.setTextSize(2);
  tw = display.textWidth(typeLabel);
  display.setCursor((800-tw)/2, 120);
  display.print(typeLabel);

  int ow = G.sqOwner[sq];

  // "Waiting for Player X..." footer — shown on all action types
  char waitTxt[40];
  snprintf(waitTxt, sizeof(waitTxt), "Waiting for Player %d...", pIdx+1);

  switch (SQ[sq].type) {
    case SQ_PROPERTY:
    case SQ_RAILROAD:
    case SQ_UTILITY: {
      if (ow == -1) {
        // Unowned: show price + hint — player card has BUY/SKIP buttons
        display.setTextColor(C_GOLD); display.setTextSize(3);
        char pr[36]; snprintf(pr, sizeof(pr), "Price: $%d  |  Rent: $%d", SQ[sq].price, SQ[sq].value);
        tw = display.textWidth(pr);
        display.setCursor((800-tw)/2, 175);
        display.print(pr);

        display.setTextColor(C_LGRAY); display.setTextSize(2);
        tw = display.textWidth(waitTxt);
        display.setCursor((800-tw)/2, 240);
        display.print(waitTxt);

        display.setTextColor(C_GRAY); display.setTextSize(1);
        tw = display.textWidth("Player card: [OK]=BUY  [RIGHT]=SKIP");
        display.setCursor((800-tw)/2, 440);
        display.print("Player card: [OK]=BUY  [RIGHT]=SKIP");
      } else if (ow == pIdx) {
        // Owned by current player — auto-advance, no player action needed
        display.setTextColor(C_GREEN); display.setTextSize(3);
        tw = display.textWidth("YOUR PROPERTY");
        display.setCursor((800-tw)/2, 180);
        display.print("YOUR PROPERTY");

        display.setTextColor(C_LGRAY); display.setTextSize(2);
        tw = display.textWidth("Tap screen or press OK");
        display.setCursor((800-tw)/2, 240);
        display.print("Tap screen or press OK");
      } else {
        // Owned by other player: rent already charged
        int rent = calcRent(sq, ow);
        display.setTextColor(C_RED); display.setTextSize(3);
        tw = display.textWidth("PAY RENT");
        display.setCursor((800-tw)/2, 175);
        display.print("PAY RENT");

        display.setTextColor(C_WHITE); display.setTextSize(3);
        char rentTxt[48];
        snprintf(rentTxt, sizeof(rentTxt), "$%d  ->  Player %d", rent, ow+1);
        tw = display.textWidth(rentTxt);
        display.setCursor((800-tw)/2, 230);
        display.print(rentTxt);

        display.setTextColor(C_LGRAY); display.setTextSize(2);
        tw = display.textWidth(waitTxt);
        display.setCursor((800-tw)/2, 290);
        display.print(waitTxt);

        display.setTextColor(C_GRAY); display.setTextSize(1);
        tw = display.textWidth("Player card: [OK]=Confirm");
        display.setCursor((800-tw)/2, 440);
        display.print("Player card: [OK]=Confirm");
      }
      break;
    }

    case SQ_TAX: {
      // Tax already charged
      display.setTextColor(C_RED); display.setTextSize(3);
      int tax = calcTaxDue(pIdx, sq);
      char taxTxt[32]; snprintf(taxTxt, sizeof(taxTxt), "Pay $%d", tax);
      tw = display.textWidth(taxTxt);
      display.setCursor((800-tw)/2, 190);
      display.print(taxTxt);

      display.setTextColor(C_LGRAY); display.setTextSize(2);
      tw = display.textWidth(waitTxt);
      display.setCursor((800-tw)/2, 254);
      display.print(waitTxt);

      display.setTextColor(C_GRAY); display.setTextSize(1);
      tw = display.textWidth("Player card: [OK]=Confirm");
      display.setCursor((800-tw)/2, 440);
      display.print("Player card: [OK]=Confirm");
      break;
    }

    case SQ_GO: {
      // $200 already credited — auto-advance after 2s
      display.setTextColor(C_GOLD); display.setTextSize(3);
      tw = display.textWidth("LANDED ON GO!  +$200");
      display.setCursor((800-tw)/2, 195);
      display.print("LANDED ON GO!  +$200");

      display.setTextColor(C_LGRAY); display.setTextSize(2);
      tw = display.textWidth("Tap screen or press OK");
      display.setCursor((800-tw)/2, 260);
      display.print("Tap screen or press OK");
      break;
    }

    case SQ_JAIL: {
      display.setTextColor(C_ORANGE); display.setTextSize(3);
      const char* jailTxt = G.P[pIdx].inJail ? "GO TO JAIL" : "JUST VISITING";
      tw = display.textWidth(jailTxt);
      display.setCursor((800-tw)/2, 195);
      display.print(jailTxt);

      display.setTextColor(C_LGRAY); display.setTextSize(2);
      tw = display.textWidth("Tap screen or press OK");
      display.setCursor((800-tw)/2, 260);
      display.print("Tap screen or press OK");
      break;
    }

    case SQ_GOTO_JAIL: {
      display.setTextColor(C_RED); display.setTextSize(3);
      tw = display.textWidth("GO TO JAIL!");
      display.setCursor((800-tw)/2, 195);
      display.print("GO TO JAIL!");

      display.setTextColor(C_LGRAY); display.setTextSize(2);
      tw = display.textWidth("Tap screen or press OK");
      display.setCursor((800-tw)/2, 260);
      display.print("Tap screen or press OK");
      break;
    }

    default: {
      // Free Parking and any other square — auto-advance
      display.setTextColor(C_LGRAY); display.setTextSize(2);
      tw = display.textWidth("No action needed");
      display.setCursor((800-tw)/2, 200);
      display.print("No action needed");

      tw = display.textWidth("Tap screen or press OK");
      display.setCursor((800-tw)/2, 240);
      display.print("Tap screen or press OK");
      break;
    }
  }

  drawRestartBtn();
  flushDisp();
}

// ── Card Screen (called blocking from processSquare) ──────────
void drawCardScreen(bool isChance, const char* text, int delta) {
  display.fillScreen(C_BG);

  // Header bar
  uint32_t hdrColor = isChance ? C_ORANGE : C_CYAN;
  display.fillRect(0, 0, 800, 80, hdrColor);
  display.setTextColor(C_WHITE); display.setTextSize(4);
  const char* hdrText = isChance ? "CHANCE" : "COMMUNITY CHEST";
  int tw = display.textWidth(hdrText);
  display.setCursor((800-tw)/2, 16);
  display.print(hdrText);

  // Card body — show immediately (no animation)
  display.fillRoundRect(80, 100, 640, 240, 24, C_WHITE);
  display.drawRoundRect(80, 100, 640, 240, 24, C_LGRAY);

  // Card text — split into two lines if too wide
  display.setTextColor(C_BLACK); display.setTextSize(3);
  int textW = display.textWidth(text);
  if (textW <= 600) {
    display.setCursor((800-textW)/2, 195);
    display.print(text);
  } else {
    int mid = strlen(text) / 2;
    while (mid > 0 && text[mid] != ' ') mid--;
    char line1[64], line2[64];
    strncpy(line1, text, mid); line1[mid] = '\0';
    strncpy(line2, text + mid + 1, 63); line2[63] = '\0';
    tw = display.textWidth(line1);
    display.setCursor((800-tw)/2, 158);
    display.print(line1);
    tw = display.textWidth(line2);
    display.setCursor((800-tw)/2, 205);
    display.print(line2);
  }

  // Effect badge (green = gain, red = loss)
  if (delta != 0) {
    uint32_t badgeColor = (delta > 0) ? C_GREEN : C_RED;
    char badge[16]; snprintf(badge, sizeof(badge), "%+d", delta);
    display.fillRoundRect(300, 360, 200, 60, 14, badgeColor);
    display.setTextColor(C_WHITE); display.setTextSize(3);
    tw = display.textWidth(badge);
    display.setCursor((800-tw)/2, 378);
    display.print(badge);
  }

  display.setTextColor(C_GRAY); display.setTextSize(1);
  tw = display.textWidth("Tap to continue");
  display.setCursor((800-tw)/2, 455);
  display.print("Tap to continue");

  flushDisp();

  // Wait for touch — no timeout
  {
    lgfx::touch_point_t tp[1];
    while (display.getTouch(tp, 1) > 0) delay(16);
    bool wasT = false;
    while (true) {
      bool t = display.getTouch(tp, 1) > 0;
      if (t && !wasT) break;
      wasT = t;
      delay(16);
    }
  }
}

// ── Auction Screen (PH_AUCTION) ──────────────────────────────
// Central bank is the auctioneer: players bid via player card.
// curBidder = -1 means no bids yet.
void drawAuctionScreen(int sq, int curBid, int curBidder, int secsLeft) {
  display.fillScreen(C_BG);

  // Header bar (yellow)
  display.fillRect(0, 0, 800, 80, C_YELLOW);
  display.setTextColor(C_BLACK); display.setTextSize(5);
  int tw = display.textWidth("AUCTION");
  display.setCursor((800-tw)/2, 14);
  display.print("AUCTION");

  TouchRect exitBtn = { 680, 15, 95, 50 };
  display.fillRoundRect(exitBtn.x, exitBtn.y, exitBtn.w, exitBtn.h, 8, C_RED);
  display.drawRoundRect(exitBtn.x, exitBtn.y, exitBtn.w, exitBtn.h, 8, C_BLACK);
  display.setTextColor(C_WHITE); display.setTextSize(2);
  tw = display.textWidth("EXIT");
  display.setCursor(exitBtn.x + (exitBtn.w - tw) / 2, exitBtn.y + 15);
  display.print("EXIT");

  // Property name
  display.setTextColor(C_WHITE); display.setTextSize(4);
  tw = display.textWidth(SQ[sq].name);
  display.setCursor((800-tw)/2, 105);
  display.print(SQ[sq].name);

  // Starting price
  display.setTextColor(C_GOLD); display.setTextSize(2);
  char priceTxt[32]; snprintf(priceTxt, sizeof(priceTxt), "Starting price: $%d", SQ[sq].price);
  tw = display.textWidth(priceTxt);
  display.setCursor((800-tw)/2, 165);
  display.print(priceTxt);

  // Current bid
  display.setTextColor(C_LGRAY); display.setTextSize(2);
  tw = display.textWidth("Current bid:");
  display.setCursor((800-tw)/2, 215);
  display.print("Current bid:");

  if (curBidder < 0) {
    display.setTextColor(C_GRAY); display.setTextSize(3);
    tw = display.textWidth("No bids yet");
    display.setCursor((800-tw)/2, 255);
    display.print("No bids yet");
  } else {
    display.setTextColor(C_GREEN); display.setTextSize(3);
    char bidTxt[40]; snprintf(bidTxt, sizeof(bidTxt), "$%d  — Player %d", curBid, curBidder+1);
    tw = display.textWidth(bidTxt);
    display.setCursor((800-tw)/2, 255);
    display.print(bidTxt);
  }

  // Countdown timer
  display.setTextColor(secsLeft <= 3 ? C_RED : C_ORANGE); display.setTextSize(3);
  char timerTxt[24]; snprintf(timerTxt, sizeof(timerTxt), "[%ds]", secsLeft);
  tw = display.textWidth(timerTxt);
  display.setCursor((800-tw)/2, 330);
  display.print(timerTxt);

  // Bottom status
  display.setTextColor(C_LGRAY); display.setTextSize(2);
  tw = display.textWidth("Waiting for bids...");
  display.setCursor((800-tw)/2, 400);
  display.print("Waiting for bids...");

  display.setTextColor(C_GRAY); display.setTextSize(1);
  tw = display.textWidth("Player card: [OK]=Bid +$50  [RIGHT]=Pass");
  display.setCursor((800-tw)/2, 455);
  display.print("Player card: [OK]=Bid +$50  [RIGHT]=Pass");

  drawRestartBtn();
  flushDisp();
}

// ── Action Result Screen (PH_ACTION_RESULT) ──────────────────
void drawActionResult() {
  display.fillScreen(C_BG);

  display.setTextColor(C_GOLD); display.setTextSize(4);
  int tw = display.textWidth("RESULT");
  display.setCursor((800-tw)/2, 55);
  display.print("RESULT");

  // Event message — split at 42 chars per line
  display.setTextColor(C_WHITE); display.setTextSize(2);
  int len = strlen(G.evtMsg);
  if (len == 0) {
    tw = display.textWidth("Turn complete");
    display.setCursor((800-tw)/2, 200);
    display.print("Turn complete");
  } else {
    const int LMAX = 42;
    for (int ln = 0, off = 0; ln < 5 && off < len; ln++, off += LMAX) {
      char row[LMAX+1];
      strncpy(row, G.evtMsg + off, LMAX); row[LMAX] = '\0';
      tw = display.textWidth(row);
      display.setCursor((800-tw)/2, 160 + ln*32);
      display.print(row);
    }
  }

  // Both player balances
  for (int p = 0; p < NUM_PLY; p++) {
    display.setTextColor(P_COLOR[p]); display.setTextSize(2);
    char s[32]; snprintf(s, sizeof(s), "%s: $%d", G.P[p].name, G.P[p].balance);
    tw = display.textWidth(s);
    display.setCursor((800-tw)/2, 360 + p*38);
    display.print(s);
  }

  display.setTextColor(C_GRAY); display.setTextSize(1);
  tw = display.textWidth("Tap screen to continue");
  display.setCursor((800-tw)/2, 455);
  display.print("Tap screen to continue");

  drawRestartBtn();
  flushDisp();
}

// ── Wait Screen (shown before game starts) ───────────────────
void drawWaitScreen() {
  display.fillScreen(C_BG);

  display.setTextColor(C_GOLD); display.setTextSize(6);
  int tw = display.textWidth("MONOPOLY");
  display.setCursor((800-tw)/2, 70);
  display.print("MONOPOLY");

  display.setTextColor(C_LGRAY); display.setTextSize(2);
  tw = display.textWidth("Waiting for players to connect...");
  display.setCursor((800-tw)/2, 185);
  display.print("Waiting for players to connect...");

  // Player slots
  for (int p = 0; p < NUM_PLY; p++) {
    int x = 130 + p * 370;
    uint32_t col = G.P[p].connected ? P_COLOR[p] : C_GRAY;
    display.fillRoundRect(x, 240, 220, 90, 12, 0x18183AUL);
    display.drawRoundRect(x, 240, 220, 90, 12, col);
    display.setTextColor(col); display.setTextSize(3);
    char lbl[8]; snprintf(lbl, sizeof(lbl), "P%d", p+1);
    tw = display.textWidth(lbl);
    display.setCursor(x+(220-tw)/2, 256);
    display.print(lbl);
    display.setTextSize(1);
    display.setCursor(x+10, 310);
    display.print(G.P[p].connected ? "CONNECTED" : "waiting...");
  }

  // Camera slot
  {
    int cx = 290;
    uint32_t camCol = cameraConnected ? C_GREEN : C_GRAY;
    display.fillRoundRect(cx, 360, 220, 70, 12, 0x18183AUL);
    display.drawRoundRect(cx, 360, 220, 70, 12, camCol);
    display.setTextColor(camCol); display.setTextSize(2);
    tw = display.textWidth("Camera");
    display.setCursor(cx+(220-tw)/2, 370);
    display.print("Camera");
    display.setTextSize(1);
    display.setCursor(cx+10, 408);
    display.print(cameraConnected ? "CONNECTED" : "waiting...");
  }

  display.setTextColor(C_GRAY); display.setTextSize(1);
  tw = display.textWidth("BLE: " BLE_NAME);
  display.setCursor((800-tw)/2, 462);
  display.print("BLE: " BLE_NAME);

  if (G.P[0].connected && G.P[1].connected && cameraConnected) {
    display.setTextColor(C_GOLD); display.setTextSize(2);
    tw = display.textWidth("All connected!  Tap screen to start");
    display.setCursor((800-tw)/2, 438);
    display.print("All connected!  Tap screen to start");
  }

  flushDisp();
}

// ── Conflict Resolution Screen (PH_CONFLICT) ────────────────
void drawConflictScreen() {
  if (currentConflict >= conflictCount) return;
  Conflict& c = conflicts[currentConflict];

  display.fillScreen(C_BG);

  // Header bar
  display.fillRect(0, 0, 800, 80, C_ORANGE);
  display.setTextColor(C_WHITE); display.setTextSize(4);
  int tw = display.textWidth("BOARD CHECK");
  display.setCursor((800-tw)/2, 16);
  display.print("BOARD CHECK");

  // Conflict description
  display.setTextColor(C_WHITE); display.setTextSize(3);
  tw = display.textWidth(c.desc);
  display.setCursor((800-tw)/2, 120);
  display.print(c.desc);

  // Detail line: Camera vs System
  display.setTextColor(C_LGRAY); display.setTextSize(2);
  char detail[80];
  snprintf(detail, sizeof(detail), "Camera: %d house %d hotel  |  System: %d house %d hotel",
    c.camHouses, c.camHotels, c.sysHouses, c.sysHotels);
  tw = display.textWidth(detail);
  display.setCursor((800-tw)/2, 180);
  display.print(detail);

  // Question
  display.setTextColor(C_GOLD); display.setTextSize(2);
  tw = display.textWidth("Confirm update?");
  display.setCursor((800-tw)/2, 240);
  display.print("Confirm update?");

  TouchRect confirmBtn = { 70, 300, 190, 70 };
  TouchRect adjustBtn  = { 305, 300, 190, 70 };
  TouchRect skipBtn    = { 540, 300, 190, 70 };

  // Confirm button
  display.fillRoundRect(confirmBtn.x, confirmBtn.y, confirmBtn.w, confirmBtn.h, 14, C_GREEN);
  display.drawRoundRect(confirmBtn.x, confirmBtn.y, confirmBtn.w, confirmBtn.h, 14, C_WHITE);
  display.setTextColor(C_WHITE); display.setTextSize(3);
  tw = display.textWidth("Confirm");
  display.setCursor(confirmBtn.x + (confirmBtn.w-tw)/2, confirmBtn.y + 18);
  display.print("Confirm");

  // Adjust button
  display.fillRoundRect(adjustBtn.x, adjustBtn.y, adjustBtn.w, adjustBtn.h, 14, C_RED);
  display.drawRoundRect(adjustBtn.x, adjustBtn.y, adjustBtn.w, adjustBtn.h, 14, C_WHITE);
  display.setTextColor(C_WHITE); display.setTextSize(3);
  tw = display.textWidth("Adjust");
  display.setCursor(adjustBtn.x + (adjustBtn.w-tw)/2, adjustBtn.y + 18);
  display.print("Adjust");

  // Skip button
  display.fillRoundRect(skipBtn.x, skipBtn.y, skipBtn.w, skipBtn.h, 14, C_ORANGE);
  display.drawRoundRect(skipBtn.x, skipBtn.y, skipBtn.w, skipBtn.h, 14, C_WHITE);
  display.setTextColor(C_WHITE); display.setTextSize(3);
  tw = display.textWidth("Skip");
  display.setCursor(skipBtn.x + (skipBtn.w-tw)/2, skipBtn.y + 18);
  display.print("Skip");

  // Counter
  display.setTextColor(C_GRAY); display.setTextSize(2);
  char counter[32];
  snprintf(counter, sizeof(counter), "Conflict %d / %d", currentConflict + 1, conflictCount);
  tw = display.textWidth(counter);
  display.setCursor((800-tw)/2, 420);
  display.print(counter);

  flushDisp();
}

// ── Calibration Screen (PH_CALIBRATE) ───────────────────────
void drawCalibrateScreen() {
  display.fillScreen(C_BG);

  // Header
  display.fillRect(0, 0, 800, 70, C_CYAN);
  display.setTextColor(C_BLACK); display.setTextSize(3);
  int tw = display.textWidth("CAMERA CALIBRATION");
  display.setCursor((800-tw)/2, 18);
  display.print("CAMERA CALIBRATION");

  display.setTextColor(C_LGRAY); display.setTextSize(2);
  tw = display.textWidth("Adjust camera offset, each tap = 2px");
  display.setCursor((800-tw)/2, 90);
  display.print("Adjust camera offset, each tap = 2px");

  // Button layout — centered cross pattern
  TouchRect upBtn    = { 335, 150, 130, 62 };
  TouchRect downBtn  = { 335, 350, 130, 62 };
  TouchRect leftBtn  = { 185, 250, 130, 62 };
  TouchRect rightBtn = { 485, 250, 130, 62 };
  TouchRect doneBtn  = { 335, 250, 130, 62 };

  // UP button
  display.fillRoundRect(upBtn.x, upBtn.y, upBtn.w, upBtn.h, 10, C_BLUE);
  display.setTextColor(C_WHITE); display.setTextSize(3);
  tw = display.textWidth("UP");
  display.setCursor(upBtn.x + (upBtn.w-tw)/2, upBtn.y + 15);
  display.print("UP");

  // DOWN button
  display.fillRoundRect(downBtn.x, downBtn.y, downBtn.w, downBtn.h, 10, C_BLUE);
  tw = display.textWidth("DOWN");
  display.setCursor(downBtn.x + (downBtn.w-tw)/2, downBtn.y + 15);
  display.print("DOWN");

  // LEFT button
  display.fillRoundRect(leftBtn.x, leftBtn.y, leftBtn.w, leftBtn.h, 10, C_BLUE);
  tw = display.textWidth("LEFT");
  display.setCursor(leftBtn.x + (leftBtn.w-tw)/2, leftBtn.y + 15);
  display.print("LEFT");

  // RIGHT button
  display.fillRoundRect(rightBtn.x, rightBtn.y, rightBtn.w, rightBtn.h, 10, C_BLUE);
  tw = display.textWidth("RIGHT");
  display.setCursor(rightBtn.x + (rightBtn.w-tw)/2, rightBtn.y + 15);
  display.print("RIGHT");

  // DONE button (center)
  display.fillRoundRect(doneBtn.x, doneBtn.y, doneBtn.w, doneBtn.h, 10, C_GREEN);
  tw = display.textWidth("DONE");
  display.setCursor(doneBtn.x + (doneBtn.w-tw)/2, doneBtn.y + 15);
  display.print("DONE");

  flushDisp();
}

// ── Confirm Restart Screen ──────────────────────────────────
void drawConfirmRestartScreen() {
  display.fillScreen(C_BG);

  display.setTextColor(C_RED); display.setTextSize(4);
  int tw = display.textWidth("RESTART GAME?");
  display.setCursor((800 - tw) / 2, 100);
  display.print("RESTART GAME?");

  display.setTextColor(C_LGRAY); display.setTextSize(2);
  tw = display.textWidth("All progress will be lost.");
  display.setCursor((800 - tw) / 2, 190);
  display.print("All progress will be lost.");

  // YES button
  display.fillRoundRect(150, 300, 200, 70, 14, C_RED);
  display.setTextColor(C_WHITE); display.setTextSize(3);
  tw = display.textWidth("YES");
  display.setCursor(150 + (200 - tw) / 2, 318);
  display.print("YES");

  // NO button
  display.fillRoundRect(450, 300, 200, 70, 14, C_GREEN);
  display.setTextColor(C_WHITE); display.setTextSize(3);
  tw = display.textWidth("NO");
  display.setCursor(450 + (200 - tw) / 2, 318);
  display.print("NO");

  flushDisp();
}

// ── Game Over Screen ─────────────────────────────────────────
void drawGameOverScreen() {
  display.fillScreen(C_BG);

  display.setTextColor(C_GOLD); display.setTextSize(6);
  int tw = display.textWidth("GAME OVER");
  display.setCursor((800-tw)/2, 60);
  display.print("GAME OVER");

  if (G.winner >= 0) {
    display.setTextColor(P_COLOR[G.winner]); display.setTextSize(4);
    char wt[32]; snprintf(wt, sizeof(wt), "%s WINS!", G.P[G.winner].name);
    tw = display.textWidth(wt);
    display.setCursor((800-tw)/2, 180);
    display.print(wt);
  }

  // Final balances
  for (int p = 0; p < NUM_PLY; p++) {
    display.setTextColor(P_COLOR[p]); display.setTextSize(2);
    char s[40];
    snprintf(s, sizeof(s), "%s: $%d", G.P[p].name, G.P[p].balance);
    tw = display.textWidth(s);
    display.setCursor((800-tw)/2, 290 + p*45);
    display.print(s);
  }

  // Event message (reason)
  display.setTextColor(C_LGRAY); display.setTextSize(2);
  tw = display.textWidth(G.evtMsg);
  display.setCursor(tw < 790 ? (800-tw)/2 : 4, 390);
  display.print(G.evtMsg);

  // "NEW GAME" button
  display.fillRoundRect(275, 440, 250, 60, 14, C_GREEN);
  display.setTextColor(C_WHITE); display.setTextSize(3);
  tw = display.textWidth("NEW GAME");
  display.setCursor((800-tw)/2, 455);
  display.print("NEW GAME");

  flushDisp();
  beep(600);
  delay(200);
  beep(300);
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  Serial0.begin(115200);
  delay(200);
  Serial0.println("=== Monopoly Central Bank ===");

  pinMode(19, OUTPUT);
  Wire.begin(15, 16);
  delay(50);

  waitI2CReady();
  stcSend(0x10);  // backlight ON

  display.init();
  display.initDMA();
  display.startWrite();
  display.fillScreen(C_BLACK);

  // Dice sprite: only covers the two-die area to minimise per-frame PSRAM writes
  diceSprite.setColorDepth(16);
  if (!diceSprite.createSprite(DICE_SW, DICE_SZ)) {
    Serial0.println("[SPRITE] Failed — dice may flicker");
  }

  randomSeed((uint32_t)esp_random());

  // Init game state
  memset(&G, 0, sizeof(G));
  for (int i = 0; i < NUM_SQ; i++) G.sqOwner[i] = -1;
  for (int p = 0; p < NUM_PLY; p++) {
    snprintf(G.P[p].name, sizeof(G.P[p].name), "Player %d", p+1);
    G.P[p].balance = START_BAL;
  }
  G.winner = -1;
  G.needRedraw = true;

  saveReady = savePrefs.begin("monopoly", false);
  if (!saveReady) Serial0.println("[SAVE] Preferences init failed");
  bool restoredGame = false;
#if !SKIP_BLE_WAIT
  restoredGame = loadGame();
#endif

#if SKIP_BLE_WAIT
  // Skip BLE entirely — go straight to game for display/dice testing
  G.P[0].connected = true;
  G.P[1].connected = true;
  cameraConnected  = true;
  initGame();
  G.P[0].connected = true;
  G.P[1].connected = true;
  enterPhase(PH_TURN_START);
#else
#if !CAMERA_ESPNOW_ONLY_TEST
  startBLE();
  delay(200);           // 让 BLE 稳定
#else
  Serial0.println("[TEST] BLE disabled; ESP-NOW camera test only");
#endif
  startCameraEspNow();  // init ESP-NOW last so it owns the final WiFi channel setup
  delay(200);           // 让 WiFi/ESP-NOW 稳定
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_err_t chErr = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (chErr != ESP_OK) Serial0.printf("[CAM] set channel after radio init failed: %d\n", chErr);
  uint8_t ch = 0; wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  Serial0.printf("[CAM] WiFi channel active: %d\n", ch);
  if (restoredGame) {
    G.needRedraw = true;
    Serial0.println("[SAVE] Restored game; waiting for BLE reconnect");
  } else {
    enterPhase(PH_WAIT_PLAYERS);
    drawWaitScreen();
  }
#endif
}

// ── Loop ─────────────────────────────────────────────────────
void loop() {
  // Process incoming BLE message (flag set by BLE task)
  if (bleRxNew) {
    bleRxNew = false;
    char msg[72]; strncpy(msg, bleRxBuf, 71); msg[71] = '\0';
    handleBLERx(msg);
  }

  updateBlePause();
  if (blePauseActive) {
    if (G.needRedraw) {
      drawBlePausedOverlay();
      G.needRedraw = false;
    }
    delay(16);
    return;
  }

  // ── Camera data processing ──────────────────────────────────
  static uint32_t lastCamDataMs = millis();
  if (camDataReady) {
    camDataReady = false;
    lastCamDataMs = millis();
    char jsonCopy[512];
    strncpy(jsonCopy, camRxBuf, sizeof(jsonCopy) - 1);
    jsonCopy[sizeof(jsonCopy) - 1] = '\0';
    parseCamJson(jsonCopy);
    detectConflicts();
    if (conflictCount > 0 &&
        G.phase != PH_CONFLICT && G.phase != PH_CALIBRATE) {
      G.savedPhase = G.phase;
      enterPhase(PH_CONFLICT);
    }
  }

  // Camera timeout: if no ESP-NOW data for 15s, mark disconnected
  if (cameraConnected && millis() - lastCamDataMs > 15000) {
    cameraConnected = false;
    G.needRedraw = true;
    Serial0.println("[CAM] No data for 15s, marked disconnected");
  }

  // Touch input
  static bool wasTouched = false;
  lgfx::touch_point_t tp[1];
  bool touched = display.getTouch(tp, 1) > 0;
  bool freshTouch = touched && !wasTouched;
  wasTouched = touched;

  // Mid-game restart: RST button in top-right corner during gameplay
  if (freshTouch && isGameplayPhase(G.phase) &&
      G.phase != PH_CONFIRM_RESTART && G.phase != PH_CONFLICT && G.phase != PH_CALIBRATE) {
    if (hitRect(tp[0].x, tp[0].y, rstBtn)) {
      beep(40);
      G.savedPhase = G.phase;  // save in case user cancels
      enterPhase(PH_CONFIRM_RESTART);
    }
  }

  switch (G.phase) {

    case PH_WAIT_PLAYERS:
      if (G.needRedraw) { drawWaitScreen(); G.needRedraw = false; }
      // In single-device mode: start once P1 (the real device) connects.
      // Auto-mark P2 as connected so the rest of the game logic is unaffected.
#if SINGLE_DEVICE_TEST
      if (G.P[0].connected && !G.P[1].connected) {
        G.P[1].connected = true;
        G.needRedraw = true;
      }
#endif
      // Transition when both players AND camera are connected
      if (G.P[0].connected && G.P[1].connected && cameraConnected) {
        sendAll("START");
        snprintf(G.evtMsg, sizeof(G.evtMsg), "All devices connected!");
        enterPhase(PH_LOBBY);
      }
      break;

    case PH_LOBBY:
      if (G.needRedraw) { drawWaitScreen(); G.needRedraw = false; }
      if (freshTouch) {
        initGame();
        G.P[0].connected = true;
        G.P[1].connected = true;
        enterPhase(PH_TURN_START);
      }
      break;

    case PH_TURN_START: {
      if (G.needRedraw) {
        // Handle jail: auto-pay $50 bail at turn start
        if (G.P[G.cur].inJail) {
          G.P[G.cur].balance -= 50;
          G.P[G.cur].inJail   = false;
          G.doublesCount[G.cur] = 0;
          G.lastRollWasDouble = false;
          snprintf(G.evtMsg, sizeof(G.evtMsg),
            "P%d paid $50 jail bail", G.cur+1);
          sendAll("ACT:BAIL:50");
          sendState();
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "TURN:%d", G.cur+1);
        sendAll(buf);
        drawTurnScreen(G.cur, false);
        G.needRedraw = false;
      }
      if (freshTouch) enterPhase(PH_WAIT_ROLL);
#if SINGLE_DEVICE_TEST
      // P2 has no human — auto-advance its turn start after 1s
      if (G.cur == 1 && phaseElapsed(1000)) enterPhase(PH_WAIT_ROLL);
#endif
      break;
    }

    case PH_WAIT_ROLL:
      if (G.needRedraw) { drawTurnScreen(G.cur, true); G.needRedraw = false; }
      // Touch the centered ROLL DICE button (x:200-600, y:358-440)
      if (freshTouch &&
          tp[0].x >= 200 && tp[0].x <= 600 &&
          tp[0].y >= 358 && tp[0].y <= 440) {
        enterPhase(PH_DICE_ANIM);
      }
#if SINGLE_DEVICE_TEST
      // Auto-roll for P2 (index 1) after 2s
      if (G.cur == 1 && phaseElapsed(2000)) {
        enterPhase(PH_DICE_ANIM);
      }
#endif
      // Heartbeat: re-broadcast TURN+STATE every 5s for P1 (real device).
      // Fixes the case where the initial TURN notify was dropped and the player
      // card is stuck — no HELLO needed, central keeps trying automatically.
      if (G.cur == 0 && millis() - G.lastResendMs >= 3000) {
        G.lastResendMs = millis();
        char hb[16]; snprintf(hb, sizeof(hb), "TURN:%d", G.cur + 1);
        sendAll(hb);
        delay(40);
        sendState();
        Serial0.println("[HEARTBEAT] Re-sent TURN+STATE in PH_WAIT_ROLL");
      }
      break;

    case PH_DICE_ANIM:
      runDiceAnim();  // blocking — sets phase via processSquare → endTurn
      G.needRedraw = true;
      break;

    case PH_WAIT_ACTION: {
      if (G.needRedraw) { drawSquareLanding(G.cur, G.P[G.cur].position); G.needRedraw = false; }
      // Resend the pending ACT message every 4s for P1 (real device) in case
      // the BLE notification was dropped and the player card never received it.
      if (G.cur == 0 && G.pendingActMsg[0] &&
          millis() - G.lastResendMs >= 3000) {
        G.lastResendMs = millis();
        sendState();
        delay(40);
        sendAll(G.pendingActMsg);
        Serial0.printf("[RESEND] %s\n", G.pendingActMsg);
      }
      // Central bank display only — no touch action buttons.
      // Player card sends BLE messages (BUY/SKIP/OK) handled in handleBLERx().
      // For info-only squares (GO/JAIL/OWN/FREE): also advance on central touch.
      if (freshTouch) {
        int sq = G.P[G.cur].position;
        SqType t = SQ[sq].type;
        bool isInfo = (t == SQ_GO || t == SQ_JAIL || t == SQ_GOTO_JAIL || t == SQ_FREE_PARK);
        if (t == SQ_PROPERTY || t == SQ_RAILROAD || t == SQ_UTILITY)
          isInfo = isInfo || (G.sqOwner[sq] == G.cur);
        if (isInfo) {
          snprintf(G.evtMsg, sizeof(G.evtMsg), "P%d: %s", G.cur+1, SQ[sq].name);
          endTurn();
        }
      }
#if SINGLE_DEVICE_TEST
      // Auto-decide for P2 (index 1) after 2.5s
      if (G.cur == 1 && phaseElapsed(2500)) {
        int sq2 = G.P[1].position;
        SqType t2 = SQ[sq2].type;
        bool buyable2 = (t2 == SQ_PROPERTY || t2 == SQ_RAILROAD || t2 == SQ_UTILITY);
        if (buyable2 && G.sqOwner[sq2] == -1) {
          if (G.P[1].balance >= SQ[sq2].price) {
            G.P[1].balance -= SQ[sq2].price;
            G.sqOwner[sq2]   = 1;
            G.P[1].owns[sq2] = true;
            snprintf(G.evtMsg, sizeof(G.evtMsg),
              "P2 (auto) bought %s $%d", SQ[sq2].name, SQ[sq2].price);
          } else {
            // Auto-skip → trigger auction
            G.auctionSq      = sq2;
            G.auctionBid     = 0;
            G.auctionBidder  = -1;
            G.auctionTimerMs = millis();
            char buf[32];
            snprintf(buf, sizeof(buf), "AUCTION:%d:%d", sq2, SQ[sq2].price);
            sendAll(buf);
            enterPhase(PH_AUCTION);
            break;
          }
        } else {
          snprintf(G.evtMsg, sizeof(G.evtMsg), "P2 (auto) %s", SQ[sq2].name);
        }
        if (G.phase == PH_WAIT_ACTION) endTurn();
      }
#endif
      break;
    }

    case PH_AUCTION: {
      // Calculate seconds remaining on the 60s bid timer
      uint32_t elapsed = millis() - G.auctionTimerMs;
      int secsLeft = elapsed >= AUCTION_DURATION_MS ? 0 : (int)((AUCTION_DURATION_MS - elapsed) / 1000);

      if (G.needRedraw || (elapsed % 1000 < 60)) {
        // Redraw each second for countdown update
        drawAuctionScreen(G.auctionSq, G.auctionBid, G.auctionBidder, secsLeft);
        G.needRedraw = false;
      }

      if (freshTouch) {
        TouchRect exitBtn = { 680, 15, 95, 50 };
        if (hitRect(tp[0].x, tp[0].y, exitBtn)) {
          beep(50);
          finishAuction();
          break;
        }
      }

      // Timer expired
      if (elapsed >= AUCTION_DURATION_MS) {
        finishAuction();
      }
      break;
    }

    case PH_WAIT_TRADE: {
      if (G.needRedraw) {
        display.fillScreen(C_BG);
        display.fillRect(0, 0, 800, 80, C_PURPLE);
        display.setTextColor(C_WHITE); display.setTextSize(4);
        int tw2 = display.textWidth("TRADE PROPOSED");
        display.setCursor((800-tw2)/2, 16);
        display.print("TRADE PROPOSED");

        display.setTextColor(C_LGRAY); display.setTextSize(2);
        char line1[64], line2[64];
        snprintf(line1, sizeof(line1), "Player %d offers: %s",
          G.tradeFrom+1, SQ[G.tradeSqOffer].name);
        snprintf(line2, sizeof(line2), "Wants: %s (Player %d's)",
          SQ[G.tradeSqWant].name, (1-G.tradeFrom)+1);
        tw2 = display.textWidth(line1);
        display.setCursor((800-tw2)/2, 160);
        display.print(line1);
        tw2 = display.textWidth(line2);
        display.setCursor((800-tw2)/2, 210);
        display.print(line2);

        display.setTextColor(C_GRAY); display.setTextSize(2);
        char waiting[48];
        snprintf(waiting, sizeof(waiting), "Waiting for Player %d...", (1-G.tradeFrom)+1);
        tw2 = display.textWidth(waiting);
        display.setCursor((800-tw2)/2, 290);
        display.print(waiting);

        display.setTextColor(C_GRAY); display.setTextSize(1);
        tw2 = display.textWidth("Player card: [OK]=Accept  [LEFT]=Reject");
        display.setCursor((800-tw2)/2, 455);
        display.print("Player card: [OK]=Accept  [LEFT]=Reject");

        drawRestartBtn();
        flushDisp();
        G.needRedraw = false;
      }
      // No timeout — wait indefinitely for P{other}:TRADE:YES or :NO
      break;
    }

    case PH_ACTION_RESULT:
      if (G.needRedraw) { drawActionResult(); G.needRedraw = false; }
      if (freshTouch) {
        advanceAfterResult();
      }
#if SINGLE_DEVICE_TEST
      // P2's result screen: auto-advance after 2s (no human to tap for P2)
      if (G.cur == 1 && phaseElapsed(2000)) {
        advanceAfterResult();
      }
#endif
      break;

    case PH_CONFIRM_RESTART:
      if (G.needRedraw) { drawConfirmRestartScreen(); G.needRedraw = false; }
      if (freshTouch) {
        int tx = tp[0].x, ty = tp[0].y;
        TouchRect yesBtn = { 150, 300, 200, 70 };
        TouchRect noBtn  = { 450, 300, 200, 70 };
        if (hitRect(tx, ty, yesBtn)) {
          beep(40);
          initGame();
          G.P[0].connected = true;
          G.P[1].connected = true;
          sendAll("RESTART");
          enterPhase(PH_TURN_START);
        } else if (hitRect(tx, ty, noBtn)) {
          beep(40);
          enterPhase(G.savedPhase);
        }
      }
      break;

    case PH_GAME_OVER:
      if (G.needRedraw) { drawGameOverScreen(); G.needRedraw = false; }
      if (freshTouch) {
        TouchRect newGameBtn = { 275, 440, 250, 60 };
        if (hitRect(tp[0].x, tp[0].y, newGameBtn)) {
          beep(40);
          initGame();
          G.P[0].connected = true;
          G.P[1].connected = true;
          sendAll("START");
          enterPhase(PH_TURN_START);
        }
      }
      break;

    case PH_CONFLICT: {
      if (G.needRedraw) { drawConflictScreen(); G.needRedraw = false; }
      if (freshTouch) {
        int tx = tp[0].x, ty = tp[0].y;
        TouchRect confirmBtn = { 70, 300, 190, 70 };
        TouchRect adjustBtn  = { 305, 300, 190, 70 };
        TouchRect skipBtn    = { 540, 300, 190, 70 };

        if (hitRect(tx, ty, confirmBtn)) {
          // Update system record to match camera
          Conflict& c = conflicts[currentConflict];
          G.sysHouses[c.cellIdx] = c.camHouses;
          G.sysHotels[c.cellIdx] = c.camHotels;
          clearIgnoredConflict(c.cellIdx);
          beep(40);
          currentConflict++;
          if (currentConflict >= conflictCount) {
            // All conflicts resolved — return to game
            enterPhase(G.savedPhase);
          } else {
            G.needRedraw = true;
            saveGame();
          }
        }
        else if (hitRect(tx, ty, adjustBtn)) {
          beep(40);
          enterPhase(PH_CALIBRATE);
        }
        else if (hitRect(tx, ty, skipBtn)) {
          Conflict& c = conflicts[currentConflict];
          ignoreCurrentConflict(c);
          beep(40);
          currentConflict++;
          if (currentConflict >= conflictCount) {
            enterPhase(G.savedPhase);
          } else {
            G.needRedraw = true;
            saveGame();
          }
        }
      }
      break;
    }

    case PH_CALIBRATE: {
      if (G.needRedraw) { drawCalibrateScreen(); G.needRedraw = false; }
      if (freshTouch) {
        int tx = tp[0].x, ty = tp[0].y;
        TouchRect upBtn    = { 335, 150, 130, 62 };
        TouchRect downBtn  = { 335, 350, 130, 62 };
        TouchRect leftBtn  = { 185, 250, 130, 62 };
        TouchRect rightBtn = { 485, 250, 130, 62 };
        TouchRect doneBtn  = { 335, 250, 130, 62 };

        if (hitRect(tx, ty, doneBtn)) {
          beep(60);
          // Return to conflict screen or game
          if (currentConflict < conflictCount) {
            enterPhase(PH_CONFLICT);
          } else {
            enterPhase(G.savedPhase);
          }
        }
        else if (hitRect(tx, ty, upBtn)) {
          sendCamCalibration("{\"dy\":-2}");
          beep(30);
        }
        else if (hitRect(tx, ty, downBtn)) {
          sendCamCalibration("{\"dy\":2}");
          beep(30);
        }
        else if (hitRect(tx, ty, leftBtn)) {
          sendCamCalibration("{\"dx\":-2}");
          beep(30);
        }
        else if (hitRect(tx, ty, rightBtn)) {
          sendCamCalibration("{\"dx\":2}");
          beep(30);
        }
      }
      break;
    }
  }

  delay(16);
}
