#include <Arduino.h>
#include <Wire.h>
#include <Seeed_Arduino_SSCMA.h>

SSCMA AI;

// ── 格子名称表 ─────────────────────────────────────────────
const char* CELL_NAMES[41] = {
  "",
  "GO",
  "Old Kent Road",
  "Community Chest",
  "Whitechapel Road",
  "Income Tax",
  "King's Cross Station",
  "The Angel Islington",
  "Chance",
  "Euston Road",
  "Pentonville Road",
  "Jail / Just Visiting",
  "Pall Mall",
  "Electric Company",
  "Whitehall",
  "Northumberland Avenue",
  "Marylebone Station",
  "Bow Street",
  "Community Chest",
  "Marlborough Street",
  "Vine Street",
  "Free Parking",
  "Strand",
  "Chance",
  "Fleet Street",
  "Trafalgar Square",
  "Fenchurch St. Station",
  "Leicester Square",
  "Coventry Street",
  "Water Works",
  "Piccadilly",
  "Go To Jail",
  "Regent Street",
  "Oxford Street",
  "Community Chest",
  "Bond Street",
  "Liverpool St. Station",
  "Chance",
  "Park Lane",
  "Super Tax",
  "Mayfair",
};

// ── 可建房格子白名单 ───────────────────────────────────────
const uint8_t BUILDABLE[] = {
  2, 4,
  7, 9, 10,
  12, 14, 15,
  17, 19, 20,
  22, 24, 25,
  27, 28, 30,
  32, 33, 35,
  38, 40
};

bool isBuildable(uint8_t cellId) {
  for (int i = 0; i < sizeof(BUILDABLE); i++) {
    if (BUILDABLE[i] == cellId) return true;
  }
  return false;
}

// ── 格子坐标表（y坐标整体-4）─────────────────────────────
struct Cell {
  uint8_t id;
  uint8_t x1, y1, x2, y2;
};

const Cell CELLS[40] = {
  { 1,  18,  30,  46,  58},
  { 2,  46,  30,  62,  58},
  { 3,  62,  30,  79,  58},
  { 4,  79,  30,  95,  58},
  { 5,  95,  30, 112,  58},
  { 6, 112,  30, 128,  58},
  { 7, 128,  30, 145,  58},
  { 8, 145,  30, 161,  58},
  { 9, 161,  30, 178,  58},
  {10, 178,  30, 194,  58},
  {11, 194,  30, 222,  58},
  {12, 194,  58, 222,  74},
  {13, 194,  74, 222,  90},
  {14, 194,  90, 222, 106},
  {15, 194, 106, 222, 122},
  {16, 194, 122, 222, 139},
  {17, 194, 139, 222, 155},
  {18, 194, 155, 222, 171},
  {19, 194, 171, 222, 187},
  {20, 194, 187, 222, 203},
  {21, 194, 203, 222, 231},
  {22, 178, 203, 194, 231},
  {23, 161, 203, 178, 231},
  {24, 145, 203, 161, 231},
  {25, 128, 203, 145, 231},
  {26, 112, 203, 128, 231},
  {27,  95, 203, 112, 231},
  {28,  79, 203,  95, 231},
  {29,  62, 203,  79, 231},
  {30,  46, 203,  62, 231},
  {31,  18, 203,  46, 231},
  {32,  18, 187,  46, 203},
  {33,  18, 171,  46, 187},
  {34,  18, 155,  46, 171},
  {35,  18, 139,  46, 155},
  {36,  18, 122,  46, 139},
  {37,  18, 106,  46, 122},
  {38,  18,  90,  46, 106},
  {39,  18,  74,  46,  90},
  {40,  18,  58,  46,  74},
};

// ── 格子状态 ──────────────────────────────────────────────
struct CellState {
  uint8_t houses;
  uint8_t hotels;
};
CellState board[41];

// ── 响应缓冲 ──────────────────────────────────────────────
char buf[2048];

// ── 辅助函数 ──────────────────────────────────────────────
uint8_t findCell(uint8_t cx, uint8_t cy) {
  for (int i = 0; i < 40; i++) {
    if (cx >= CELLS[i].x1 && cx < CELLS[i].x2 &&
        cy >= CELLS[i].y1 && cy < CELLS[i].y2) {
      return CELLS[i].id;
    }
  }
  return 0;
}

void readResponse(int timeout_ms) {
  memset(buf, 0, sizeof(buf));
  int len = 0;
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    int avail = AI.available();
    if (avail > 0) {
      len += AI.read(buf + len, min(avail, (int)(sizeof(buf) - len - 1)));
    }
    delay(10);
  }
}

void parseBoxes(const char* json) {
  memset(board, 0, sizeof(board));

  const char* p = strstr(json, "\"boxes\":");
  if (!p) return;
  p += 8;

  while (*p && *p != '[') p++;
  if (!*p) return;
  p++;

  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (*p == ']') break;

    if (*p == '[') {
      p++;
      int vals[6];
      int vi = 0;
      while (*p && vi < 6) {
        while (*p == ' ') p++;
        if (*p == ']') break;
        int num = 0;
        while (*p >= '0' && *p <= '9') {
          num = num * 10 + (*p - '0');
          p++;
        }
        vals[vi++] = num;
        while (*p == ' ') p++;
        if (*p == ',') p++;
      }
      if (*p == ']') p++;

      if (vi == 6) {
        int x = vals[0], y = vals[1], w = vals[2], h = vals[3];
        int score = vals[4], cls = vals[5];
        if (score >= 40) {
          uint8_t cx = x + w / 2;
          uint8_t cy = y + h / 2;
          uint8_t cellId = findCell(cx, cy);
          if (cellId > 0 && isBuildable(cellId)) {
            if (cls == 0) board[cellId].houses++;
            else if (cls == 1) board[cellId].hotels++;
          }
        }
      }
    } else {
      p++;
    }
  }
}

void printBoard() {
  bool anyFound = false;
  for (int i = 1; i <= 40; i++) {
    if (board[i].houses > 0 || board[i].hotels > 0) {
      anyFound = true;
      Serial.print(CELL_NAMES[i]);
      Serial.print(": ");
      if (board[i].hotels > 0) {
        Serial.print("hotel x");
        Serial.print(board[i].hotels);
        Serial.print(" ");
      }
      if (board[i].houses > 0) {
        Serial.print("house x");
        Serial.print(board[i].houses);
      }
      Serial.println();
    }
  }
  if (!anyFound) Serial.println("(无检测结果)");
  Serial.println("---");
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(100);
  AI.begin();
  delay(200);

  const char cfg[] = "AT+TSCORE=40\r\n";
  AI.write(cfg, strlen(cfg));
  readResponse(1000);

  const char iou[] = "AT+TIOU=30\r\n";
  AI.write(iou, strlen(iou));
  readResponse(1000);

  Serial.println("系统就绪，开始识别...");
}

// ── Loop ──────────────────────────────────────────────────
void loop() {
  const char cmd[] = "AT+INVOKE=1,0,1\r\n";
  AI.write(cmd, strlen(cmd));
  readResponse(2000);

  char* result = strstr(buf, "\"boxes\":");
  if (result) {
    parseBoxes(result);
    printBoard();
  }

  delay(200);
}
