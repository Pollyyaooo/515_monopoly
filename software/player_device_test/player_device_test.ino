// =============================================================
// player_device_test.ino
// Breadboard prototype for Monopoly Player Device
// Hardware: Seeed XIAO ESP32S3
//   - SSD1351 128x128 color OLED (hardware SPI)
//   - PN532 NFC reader (I2C)
//   - 5 push buttons (active LOW, internal pull-up)
// =============================================================

// ─────────────────────────────────────────────────────────────
// [1] INCLUDES
// ─────────────────────────────────────────────────────────────
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <Adafruit_PN532.h>

// ─────────────────────────────────────────────────────────────
// [2] PIN DEFINES — OLED SPI
// XIAO ESP32S3: D10=GPIO9 (MOSI), D8=GPIO7 (SCK)
// RST is tied to 3.3V on the module (hardware reset), no GPIO needed
// ─────────────────────────────────────────────────────────────
#define OLED_CS   D1   // GPIO1
#define OLED_DC   D2   // GPIO2
// MOSI and SCK are handled by hardware SPI (D10, D8)

// ─────────────────────────────────────────────────────────────
// [3] PIN DEFINES — PN532 I2C
// SDA = D4 (GPIO5), SCL = D5 (GPIO6) — hardware I2C
// Set PN532 DIP switch: SW1=OFF, SW2=ON for I2C mode
// ─────────────────────────────────────────────────────────────
// Wire uses hardware I2C pins automatically on XIAO ESP32S3

// ─────────────────────────────────────────────────────────────
// [4] PIN DEFINES — 5 BUTTONS (active LOW, internal pull-up)
// ─────────────────────────────────────────────────────────────
#define BTN_UP    D0   // GPIO0
#define BTN_DOWN  D3   // GPIO3  (freed after RST→3.3V)
#define BTN_LEFT  D6   // GPIO43
#define BTN_RIGHT D7   // GPIO44
#define BTN_OK    D9   // GPIO8  (MISO pin, OLED is write-only)

// ─────────────────────────────────────────────────────────────
// [5] DISPLAY & NFC OBJECT INIT
// ─────────────────────────────────────────────────────────────
Adafruit_SSD1351 display(128, 128, &SPI, OLED_CS, OLED_DC, /*RST=*/-1);
Adafruit_PN532   nfc(/*IRQ=*/-1, /*RESET=*/-1);  // I2C mode, no IRQ/RST pins needed

// ─────────────────────────────────────────────────────────────
// [6] COLOR CONSTANTS (RGB565)
// ─────────────────────────────────────────────────────────────
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_YELLOW  0xFFE0
#define C_GREEN   0x07E0
#define C_RED     0xF800
#define C_BLUE    0x001F
#define C_CYAN    0x07FF
#define C_MAGENTA 0xF81F
#define C_GRAY    0x8410
#define C_ORANGE  0xFD20
#define C_PURPLE  0x780F
#define C_LBLUE   0x5D1C  // light blue

// ─────────────────────────────────────────────────────────────
// [7] PAGE ENUM
// ─────────────────────────────────────────────────────────────
enum Page {
  PAGE_HOME,
  PAGE_ACTION,
  PAGE_PROPERTIES,
  PAGE_CARDS,
  PAGE_NFC_SCAN
};

// ─────────────────────────────────────────────────────────────
// [8] STRUCTS
// ─────────────────────────────────────────────────────────────
struct Property {
  const char* name;
  uint16_t    colorRGB565;
  int         price;
  int         houses;   // 0-4 = houses, 5 = hotel
  int         owner;    // -1 = unowned, 0-3 = player index
};

struct Player {
  const char* name;
  int         balance;
  int         position;    // index into squares[]
  int         ownedProps[6];  // indices into properties[] (-1 = empty)
  int         ownedPropCount;
  int         heldCards[4];   // simple card IDs (-1 = empty)
  int         heldCardCount;
};

enum SquareType {
  SQ_PROPERTY,
  SQ_RAILROAD,
  SQ_UTILITY,
  SQ_TAX,
  SQ_SPECIAL
};

struct Square {
  const char* name;
  SquareType  type;
  int         price;
  int         rentBase;
  int         owner;    // -1 = unowned / N/A
};

struct NfcCard {
  uint8_t uid[4];
  int     squareIdx;  // index into squares[]
};

struct GameState {
  Player players[4];
  int    currentTurnIdx;
  int    totalPlayers;
};

// ─────────────────────────────────────────────────────────────
// [9] MOCK DATA
// ─────────────────────────────────────────────────────────────

// 10 board squares (mock subset)
Square squares[10] = {
  // idx  name                    type           price  rent  owner
  { "Go",               SQ_SPECIAL,   0,    0,   -1 },  // 0
  { "Mediterranean Ave",SQ_PROPERTY,  60,   2,   -1 },  // 1
  { "Baltic Ave",       SQ_PROPERTY,  60,   4,   -1 },  // 2
  { "Reading Railroad", SQ_RAILROAD,  200,  25,  -1 },  // 3
  { "Oriental Ave",     SQ_PROPERTY,  100,  6,   -1 },  // 4
  { "Jail/Visiting",    SQ_SPECIAL,   0,    0,   -1 },  // 5
  { "New York Ave",     SQ_PROPERTY,  200,  16,  -1 },  // 6
  { "Park Place",       SQ_PROPERTY,  350,  35,  1  },  // 7  owned by Bob (idx 1)
  { "Luxury Tax",       SQ_TAX,       0,    75,  -1 },  // 8
  { "Indiana Ave",      SQ_PROPERTY,  220,  18,  0  },  // 9  owned by Alice (idx 0)
};

// 5 properties with color swatches
// Indices match what players[].ownedProps[] reference
Property properties[5] = {
  // name               color      price  houses  owner
  { "Illinois Ave",  C_RED,     240,   3,   0 },  // 0  Alice
  { "Indiana Ave",   C_RED,     220,   2,   0 },  // 1  Alice
  { "Boardwalk",     C_BLUE,    400,   5,   1 },  // 2  Bob (hotel=5)
  { "Park Place",    C_BLUE,    350,   0,   1 },  // 3  Bob
  { "New York Ave",  C_ORANGE,  200,   0,  -1 },  // 4  unowned
};

// NFC UID → square index mapping (8 cards)
NfcCard nfcCards[8] = {
  { { 0xA1, 0xB2, 0xC3, 0xD4 }, 0 },  // Card A → Go
  { { 0x11, 0x22, 0x33, 0x44 }, 1 },  // Card B → Mediterranean Ave
  { { 0xAA, 0xBB, 0xCC, 0xDD }, 3 },  // Card C → Reading Railroad
  { { 0x01, 0x02, 0x03, 0x04 }, 7 },  // Card D → Park Place (Bob's)
  { { 0xDE, 0xAD, 0xBE, 0xEF }, 8 },  // Card E → Luxury Tax
  { { 0xCA, 0xFE, 0x00, 0x01 }, 5 },  // Card F → Jail
  { { 0x12, 0x34, 0x56, 0x78 }, 6 },  // Card G → New York Ave
  { { 0xFF, 0xEE, 0xDD, 0xCC }, 9 },  // Card H → Indiana Ave (Alice's)
};
const int NFC_CARD_COUNT = 8;

// Card name lookup (simple)
const char* cardName(int cardId) {
  if (cardId == 0) return "Get Out of Jail Free";
  return "Unknown Card";
}

// Game state with 4 players
GameState game;

void initMockData() {
  game.totalPlayers   = 4;
  game.currentTurnIdx = 1;  // Bob's turn (Alice is "you")

  // Alice — player 0
  game.players[0] = {
    "Alice", 1450, 7,       // position = Park Place (index 7, owned by Bob)
    { 0, 1, -1, -1, -1, -1 }, 2,  // owns Illinois Ave & Indiana Ave
    { 0, -1, -1, -1 }, 1           // holds 1 Get Out of Jail Free card
  };

  // Bob — player 1
  game.players[1] = {
    "Bob", 980, 7,           // position = Park Place (own square)
    { 2, 3, -1, -1, -1, -1 }, 2,  // owns Boardwalk & Park Place
    { -1, -1, -1, -1 }, 0
  };

  // Carol — player 2
  game.players[2] = {
    "Carol", 2100, 0,        // position = Go
    { -1, -1, -1, -1, -1, -1 }, 0,
    { -1, -1, -1, -1 }, 0
  };

  // Dave — player 3
  game.players[3] = {
    "Dave", 650, 5,          // position = Jail
    { -1, -1, -1, -1, -1, -1 }, 0,
    { -1, -1, -1, -1 }, 0
  };
}

// ─────────────────────────────────────────────────────────────
// [10] GLOBAL STATE
// ─────────────────────────────────────────────────────────────
Page currentPage = PAGE_HOME;
bool needRedraw   = true;
int  propScrollOffset = 0;   // for PROPERTIES page scrolling

// ─────────────────────────────────────────────────────────────
// [11] NFC HANDLER
// ─────────────────────────────────────────────────────────────

// Returns square index if uid matches a card, -1 otherwise
int lookupUID(uint8_t* uid, uint8_t uidLen) {
  if (uidLen != 4) return -1;
  for (int i = 0; i < NFC_CARD_COUNT; i++) {
    bool match = true;
    for (int b = 0; b < 4; b++) {
      if (nfcCards[i].uid[b] != uid[b]) { match = false; break; }
    }
    if (match) return nfcCards[i].squareIdx;
  }
  return -1;
}

void scanNFC() {
  uint8_t uid[7];
  uint8_t uidLen = 0;
  // Timeout = 50ms so it's non-blocking enough for the main loop
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) {
    Serial.print("[NFC] UID: ");
    for (int i = 0; i < uidLen; i++) {
      Serial.printf("%02X ", uid[i]);
    }
    Serial.println();

    int sq = lookupUID(uid, uidLen);
    if (sq >= 0) {
      game.players[game.currentTurnIdx].position = sq;
      currentPage = PAGE_ACTION;
      needRedraw  = true;
      Serial.printf("[NFC] -> square %d (%s)\n", sq, squares[sq].name);
    } else {
      // Unknown card: print UID for mapping table setup
      Serial.println("[NFC] Unknown card — add UID to nfcCards[] table");
    }
  }
}

// ─────────────────────────────────────────────────────────────
// [12] BUTTON HANDLER
// ─────────────────────────────────────────────────────────────
#define DEBOUNCE_MS  50
#define LONGPRESS_MS 800

struct ButtonState {
  uint8_t  pin;
  bool     lastRaw;       // raw GPIO reading (LOW = pressed)
  bool     debounced;     // debounced state
  uint32_t lastChangeMs;  // when lastRaw changed
  uint32_t pressStartMs;  // when debounced press began
  bool     pressActive;   // currently considered held
  bool     longFired;     // long-press event already sent
};

ButtonState buttons[5];

void initButtons() {
  uint8_t pins[5] = { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_OK };
  for (int i = 0; i < 5; i++) {
    pinMode(pins[i], INPUT_PULLUP);
    buttons[i] = { pins[i], HIGH, HIGH, 0, 0, false, false };
  }
}

// Call from loop(). Returns true if any event fired.
// Events are dispatched directly to handleButtonEvent().
void processButtons();

enum ButtonEvent { EVT_NONE, EVT_SHORT, EVT_LONG };

void handleButtonEvent(uint8_t pin, ButtonEvent evt);

void processButtons() {
  uint32_t now = millis();
  for (int i = 0; i < 5; i++) {
    bool raw = digitalRead(buttons[i].pin);  // LOW = pressed

    // Detect raw change → start debounce timer
    if (raw != buttons[i].lastRaw) {
      buttons[i].lastRaw      = raw;
      buttons[i].lastChangeMs = now;
    }

    // Debounce: only accept stable signal after DEBOUNCE_MS
    if ((now - buttons[i].lastChangeMs) >= DEBOUNCE_MS) {
      bool stable = raw;  // raw has been stable for DEBOUNCE_MS

      if (stable == LOW && !buttons[i].pressActive) {
        // Fresh press
        buttons[i].pressActive  = true;
        buttons[i].pressStartMs = now;
        buttons[i].longFired    = false;

      } else if (stable == HIGH && buttons[i].pressActive) {
        // Button released
        buttons[i].pressActive = false;
        uint32_t held = now - buttons[i].pressStartMs;
        if (!buttons[i].longFired) {
          // Released before long-press threshold → short press
          handleButtonEvent(buttons[i].pin, EVT_SHORT);
        }

      } else if (stable == LOW && buttons[i].pressActive && !buttons[i].longFired) {
        // Still held — check for long press
        if ((now - buttons[i].pressStartMs) >= LONGPRESS_MS) {
          buttons[i].longFired = true;
          handleButtonEvent(buttons[i].pin, EVT_LONG);
        }
      }

      buttons[i].debounced = (stable == LOW);
    }
  }
}

void handleButtonEvent(uint8_t pin, ButtonEvent evt) {
  if (evt == EVT_NONE) return;

  Serial.printf("[BTN] pin %d %s\n", pin, evt == EVT_SHORT ? "SHORT" : "LONG");

  // Long-press OK on any page → advance to next turn
  if (pin == BTN_OK && evt == EVT_LONG) {
    game.currentTurnIdx = (game.currentTurnIdx + 1) % game.totalPlayers;
    Serial.printf("[TURN] Now player %d (%s)\n",
                  game.currentTurnIdx,
                  game.players[game.currentTurnIdx].name);
    currentPage = PAGE_HOME;
    needRedraw  = true;
    return;
  }

  switch (currentPage) {

    case PAGE_HOME:
      if (pin == BTN_RIGHT && evt == EVT_SHORT) { currentPage = PAGE_ACTION;     needRedraw = true; }
      if (pin == BTN_UP    && evt == EVT_SHORT) { currentPage = PAGE_PROPERTIES; needRedraw = true; propScrollOffset = 0; }
      if (pin == BTN_DOWN  && evt == EVT_SHORT) { currentPage = PAGE_CARDS;      needRedraw = true; }
      break;

    case PAGE_ACTION:
      if (pin == BTN_LEFT && evt == EVT_SHORT) { currentPage = PAGE_HOME; needRedraw = true; }
      if (pin == BTN_OK   && evt == EVT_SHORT) {
        Serial.println("[ACTION] Confirmed action (mock — no server call)");
        needRedraw = true;
      }
      break;

    case PAGE_PROPERTIES:
      if (pin == BTN_LEFT && evt == EVT_SHORT) { currentPage = PAGE_HOME; needRedraw = true; }
      if (pin == BTN_UP   && evt == EVT_SHORT) {
        if (propScrollOffset > 0) { propScrollOffset--; needRedraw = true; }
      }
      if (pin == BTN_DOWN && evt == EVT_SHORT) {
        int maxScroll = max(0, game.players[0].ownedPropCount - 4);
        if (propScrollOffset < maxScroll) { propScrollOffset++; needRedraw = true; }
      }
      break;

    case PAGE_CARDS:
      if (pin == BTN_LEFT && evt == EVT_SHORT) { currentPage = PAGE_HOME; needRedraw = true; }
      break;

    case PAGE_NFC_SCAN:
      // NFC page exits automatically on card scan; LEFT cancels
      if (pin == BTN_LEFT && evt == EVT_SHORT) { currentPage = PAGE_HOME; needRedraw = true; }
      break;
  }
}

// ─────────────────────────────────────────────────────────────
// [13] DRAW FUNCTIONS
// ─────────────────────────────────────────────────────────────

// Shared header bar
void drawHeader(const char* title) {
  display.fillRect(0, 0, 128, 14, C_BLUE);
  display.setTextColor(C_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 3);
  display.print(title);
}

// ── drawHome ──────────────────────────────────────────────────
void drawHome() {
  display.fillScreen(C_BLACK);
  drawHeader("MONOPOLY");

  Player& p = game.players[0];  // "you" is always player 0

  display.setTextSize(1);

  // Player name
  display.setTextColor(C_YELLOW);
  display.setCursor(4, 18);
  display.print("P1: ");
  display.print(p.name);

  // Balance
  display.setTextColor(C_GREEN);
  display.setCursor(4, 30);
  display.print("$: ");
  display.print(p.balance);

  // Position
  display.setTextColor(C_WHITE);
  display.setCursor(4, 42);
  display.print("@ ");
  // Truncate to fit 128px (max ~18 chars at textSize=1)
  char posName[19];
  strncpy(posName, squares[p.position].name, 18);
  posName[18] = '\0';
  display.print(posName);

  // Property count + card count
  display.setTextColor(C_CYAN);
  display.setCursor(4, 54);
  display.print("[Prop:");
  display.print(p.ownedPropCount);
  display.print("][Card:");
  display.print(p.heldCardCount);
  display.print("]");

  // Turn indicator
  display.setCursor(4, 68);
  if (game.currentTurnIdx == 0) {
    display.setTextColor(C_YELLOW);
    display.print("Your turn!");
  } else {
    display.setTextColor(C_GRAY);
    display.print(game.players[game.currentTurnIdx].name);
    display.print("'s turn...");
  }

  // Navigation hints
  display.setTextColor(C_GRAY);
  display.setCursor(4, 116);
  display.print("R:Action U:Prop D:Cards");
}

// ── drawAction ────────────────────────────────────────────────
void drawAction() {
  display.fillScreen(C_BLACK);
  drawHeader("ACTION");

  Player& p  = game.players[game.currentTurnIdx];
  Square& sq = squares[p.position];

  display.setTextColor(C_WHITE);
  display.setTextSize(1);
  display.setCursor(4, 18);
  display.print(sq.name);

  display.setCursor(4, 32);
  switch (sq.type) {
    case SQ_PROPERTY:
      if (sq.owner == -1) {
        display.setTextColor(C_GREEN);
        display.print("Buy $");  display.println(sq.price);
        display.setTextColor(C_YELLOW);
        display.setCursor(4, 44); display.print("Auction");
        display.setCursor(4, 56); display.print("Skip");
      } else if (sq.owner == game.currentTurnIdx) {
        display.setTextColor(C_CYAN);
        display.print("Build / Mortgage");
      } else {
        int rent = sq.rentBase;
        display.setTextColor(C_RED);
        display.print("Pay Rent $"); display.print(rent);
      }
      break;
    case SQ_RAILROAD:
      if (sq.owner == -1) {
        display.setTextColor(C_GREEN);
        display.print("Buy $"); display.print(sq.price);
      } else if (sq.owner != game.currentTurnIdx) {
        display.setTextColor(C_RED);
        display.print("Pay $"); display.print(sq.rentBase);
      } else {
        display.setTextColor(C_CYAN);
        display.print("Your Railroad");
      }
      break;
    case SQ_TAX:
      display.setTextColor(C_RED);
      display.print("Pay Tax $"); display.print(sq.rentBase);
      break;
    case SQ_SPECIAL:
      if (p.position == 5) {  // Jail
        display.setTextColor(C_ORANGE);
        display.println("Pay $50");
        display.setCursor(4, 44);
        display.print("Use Card");
        display.setCursor(4, 56);
        display.print("Roll Doubles");
      } else if (p.position == 0) {  // Go
        display.setTextColor(C_GREEN);
        display.print("Collect $200");
      } else {
        display.setTextColor(C_WHITE);
        display.print("No action");
      }
      break;
    default:
      display.print("--");
      break;
  }

  display.setTextColor(C_GRAY);
  display.setCursor(4, 116);
  display.print("OK:Confirm  L:Back");
}

// ── drawProperties ────────────────────────────────────────────
void drawProperties() {
  display.fillScreen(C_BLACK);
  drawHeader("PROPERTIES");

  Player& p = game.players[0];  // always show Alice's properties
  if (p.ownedPropCount == 0) {
    display.setTextColor(C_GRAY);
    display.setCursor(4, 40);
    display.print("No properties");
    return;
  }

  const int ROWS   = 4;
  const int ROW_H  = 22;
  const int START_Y = 18;

  for (int r = 0; r < ROWS; r++) {
    int propIdx = propScrollOffset + r;
    if (propIdx >= p.ownedPropCount) break;

    int pi = p.ownedProps[propIdx];
    if (pi < 0) continue;
    Property& prop = properties[pi];

    int y = START_Y + r * ROW_H;

    // Color swatch 10x10
    display.fillRect(4, y + 1, 10, 10, prop.colorRGB565);

    // Property name
    display.setTextColor(C_WHITE);
    display.setTextSize(1);
    display.setCursor(18, y + 2);
    // Truncate name to ~12 chars
    char nm[13];
    strncpy(nm, prop.name, 12);
    nm[12] = '\0';
    display.print(nm);

    // House / hotel icons (small squares)
    int houseX = 90;
    for (int h = 0; h < min(prop.houses, 4); h++) {
      display.fillRect(houseX + h * 7, y + 2, 5, 5, C_GREEN);
    }
    if (prop.houses == 5) {  // hotel
      display.fillRect(houseX, y + 2, 8, 7, C_RED);
    }
  }

  // Scroll indicator
  if (p.ownedPropCount > ROWS) {
    display.setTextColor(C_GRAY);
    display.setCursor(4, 116);
    display.print("U/D:scroll  L:Back");
  } else {
    display.setTextColor(C_GRAY);
    display.setCursor(4, 116);
    display.print("L:Back");
  }
}

// ── drawCards ─────────────────────────────────────────────────
void drawCards() {
  display.fillScreen(C_BLACK);
  drawHeader("CARDS");

  Player& p = game.players[0];
  if (p.heldCardCount == 0) {
    display.setTextColor(C_GRAY);
    display.setCursor(4, 40);
    display.print("No cards held");
  } else {
    for (int i = 0; i < p.heldCardCount; i++) {
      display.setTextColor(C_YELLOW);
      display.setCursor(4, 20 + i * 14);
      display.print(cardName(p.heldCards[i]));
      display.print(" x1");
    }
  }

  display.setTextColor(C_GRAY);
  display.setCursor(4, 116);
  display.print("L:Back");
}

// ── drawNfcPrompt ─────────────────────────────────────────────
void drawNfcPrompt() {
  display.fillScreen(C_BLACK);
  drawHeader("NFC SCAN");

  display.setTextColor(C_CYAN);
  display.setTextSize(1);
  display.setCursor(10, 40);
  display.print("Scan your piece...");

  display.setTextColor(C_GRAY);
  display.setCursor(4, 116);
  display.print("L:Cancel");
}

// ─────────────────────────────────────────────────────────────
// [14] MAIN DRAW DISPATCHER
// ─────────────────────────────────────────────────────────────
void redrawScreen() {
  switch (currentPage) {
    case PAGE_HOME:       drawHome();       break;
    case PAGE_ACTION:     drawAction();     break;
    case PAGE_PROPERTIES: drawProperties(); break;
    case PAGE_CARDS:      drawCards();      break;
    case PAGE_NFC_SCAN:   drawNfcPrompt();  break;
  }
}

// ─────────────────────────────────────────────────────────────
// [15] SETUP
// ─────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[BOOT] player_device_test starting...");

  // OLED
  display.begin();
  display.setRotation(0);
  display.fillScreen(C_BLACK);
  display.setTextWrap(false);
  Serial.println("[OLED] OK");

  // PN532 (I2C)
  Wire.begin();
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("[NFC] PN532 not found! Check wiring & I2C mode switch.");
    display.fillScreen(C_BLACK);
    display.setTextColor(C_RED);
    display.setCursor(4, 20);
    display.print("PN532 not found!");
    display.setCursor(4, 34);
    display.print("Check I2C wiring");
    // Don't halt — continue without NFC for UI testing
  } else {
    nfc.SAMConfig();
    Serial.printf("[NFC] PN532 firmware v%d.%d\n",
                  (versiondata >> 16) & 0xFF,
                  (versiondata >>  8) & 0xFF);
  }

  // Buttons
  initButtons();
  Serial.println("[BTN] 5 buttons initialized");

  // Mock game data
  initMockData();
  Serial.println("[GAME] Mock data loaded");

  needRedraw = true;
}

// ─────────────────────────────────────────────────────────────
// [16] LOOP
// ─────────────────────────────────────────────────────────────
void loop() {
  // NFC has priority — any page can be interrupted by a card scan
  scanNFC();

  // Button events
  processButtons();

  // Only repaint when something changed
  if (needRedraw) {
    redrawScreen();
    needRedraw = false;
  }
}
