#include <Arduino.h>
#include <Wire.h>
#include <Seeed_Arduino_SSCMA.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_now.h>

SSCMA AI;

// ── 坐标偏移（校准用）─────────────────────────────────────
int offset_x = 0;
int offset_y = 0;

// ── ESP-NOW ───────────────────────────────────────────────
// CrowPanel Central STA MAC from central serial log.
uint8_t centralMAC[] = {0x98, 0x88, 0xE0, 0x13, 0xFA, 0x2C};

volatile bool espNowReady = false;
static const uint8_t ESPNOW_CHANNEL = 1;

// ESP-NOW 发送回调（兼容 Arduino ESP32 v2 (IDF4) 和 v3 (IDF5)）
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void onDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
#else
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
#endif
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("[ESP-NOW] Send FAILED");
  }
}

// ESP-NOW 接收回调（校准指令 from CrowPanel）
void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len <= 0 || len >= 256) return;
  char msg[256];
  memcpy(msg, data, len);
  msg[len] = '\0';

  Serial.print("收到校准指令: ");
  Serial.println(msg);

  const char* dx_pos = strstr(msg, "\"dx\":");
  const char* dy_pos = strstr(msg, "\"dy\":");

  if (dx_pos) {
    dx_pos += 5;
    int sign = 1;
    if (*dx_pos == '-') { sign = -1; dx_pos++; }
    int num = 0;
    while (*dx_pos >= '0' && *dx_pos <= '9') { num = num * 10 + (*dx_pos - '0'); dx_pos++; }
    offset_x += sign * num;
    Serial.print("offset_x 更新为: ");
    Serial.println(offset_x);
  }

  if (dy_pos) {
    dy_pos += 5;
    int sign = 1;
    if (*dy_pos == '-') { sign = -1; dy_pos++; }
    int num = 0;
    while (*dy_pos >= '0' && *dy_pos <= '9') { num = num * 10 + (*dy_pos - '0'); dy_pos++; }
    offset_y += sign * num;
    Serial.print("offset_y 更新为: ");
    Serial.println(offset_y);
  }
}

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

// ── 格子坐标表 ─────────────────────────────────────────────
struct Cell {
  uint8_t id;
  uint8_t x1, y1, x2, y2;
};

const Cell CELLS[40] = {
  { 1,  17,  27,  45,  55},
  { 2,  45,  27,  62,  55},
  { 3,  62,  27,  78,  55},
  { 4,  78,  27,  95,  55},
  { 5,  95,  27, 112,  55},
  { 6, 112,  27, 128,  55},
  { 7, 128,  27, 145,  55},
  { 8, 145,  27, 162,  55},
  { 9, 162,  27, 178,  55},
  {10, 178,  27, 195,  55},
  {11, 195,  27, 223,  55},
  {12, 195,  55, 223,  71},
  {13, 195,  71, 223,  87},
  {14, 195,  87, 223, 104},
  {15, 195, 104, 223, 120},
  {16, 195, 120, 223, 136},
  {17, 195, 136, 223, 152},
  {18, 195, 152, 223, 169},
  {19, 195, 169, 223, 185},
  {20, 195, 185, 223, 201},
  {21, 195, 201, 223, 229},
  {22, 178, 201, 195, 229},
  {23, 162, 201, 178, 229},
  {24, 145, 201, 162, 229},
  {25, 128, 201, 145, 229},
  {26, 112, 201, 128, 229},
  {27,  95, 201, 112, 229},
  {28,  78, 201,  95, 229},
  {29,  62, 201,  78, 229},
  {30,  45, 201,  62, 229},
  {31,  17, 201,  45, 229},
  {32,  17, 185,  45, 201},
  {33,  17, 169,  45, 185},
  {34,  17, 152,  45, 169},
  {35,  17, 136,  45, 152},
  {36,  17, 120,  45, 136},
  {37,  17, 104,  45, 120},
  {38,  17,  87,  45, 104},
  {39,  17,  71,  45,  87},
  {40,  17,  55,  45,  71},
};

// ── 格子状态 ──────────────────────────────────────────────
struct CellState {
  uint8_t houses;
  uint8_t hotels;
};
CellState board[41];

// ── 响应缓冲 ──────────────────────────────────────────────
char buf[2048];

// （BLE 回调已移除 — 校准接收在顶部 onDataRecv 中处理）

// ── 辅助函数 ──────────────────────────────────────────────
uint8_t findCell(int cx, int cy) {
  // 加入偏移量
  cx -= offset_x;
  cy -= offset_y;
  if (cx < 0 || cy < 0 || cx > 240 || cy > 240) return 0;
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
          int cx = x + w / 2;
          int cy = y + h / 2;
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

// 把棋盘状态打包成 JSON 通过 ESP-NOW 分包发给 CrowPanel
// 每包 ≤ 250 字节，格式: {"p":1,"t":3,"cells":[...]}
// p = 当前包序号(从1开始), t = 总包数
void sendBoardState() {
  if (!espNowReady) return;

  // 先收集所有有房屋/酒店的格子
  struct CellEntry { int id; int houses; int hotels; };
  CellEntry entries[40];
  int count = 0;
  for (int i = 1; i <= 40; i++) {
    if (board[i].houses > 0 || board[i].hotels > 0) {
      entries[count++] = {i, board[i].houses, board[i].hotels};
    }
  }

  if (count == 0) {
    // 没有任何建筑，发空包
    const char* empty = "{\"p\":1,\"t\":1,\"cells\":[]}";
    esp_now_send(centralMAC, (uint8_t*)empty, strlen(empty));
    Serial.println("[ESP-NOW] 发送: (empty board)");
    return;
  }

  // 计算需要几个包：每包预留 30 字节给 header {"p":X,"t":X,"cells":[]}
  // 每个 cell 最多约 30 字节: {"id":40,"houses":4,"hotels":1}
  const int MAX_PAYLOAD = 250;
  const int HEADER_RESERVE = 30;

  // 先做一次扫描，按包拆分
  int packets[10];   // packets[i] = 第 i 包的起始 entry index
  int numPackets = 0;
  int idx = 0;
  while (idx < count) {
    packets[numPackets++] = idx;
    String test = "";
    bool first = true;
    while (idx < count) {
      String cell = "{\"id\":";
      cell += entries[idx].id;
      if (entries[idx].hotels > 0) { cell += ",\"hotels\":"; cell += entries[idx].hotels; }
      if (entries[idx].houses > 0) { cell += ",\"houses\":"; cell += entries[idx].houses; }
      cell += "}";
      int newLen = test.length() + (first ? 0 : 1) + cell.length();
      if (HEADER_RESERVE + newLen + 2 > MAX_PAYLOAD) break;  // +2 for []
      if (!first) test += ",";
      test += cell;
      first = false;
      idx++;
    }
  }

  // 逐包发送
  for (int p = 0; p < numPackets; p++) {
    int start = packets[p];
    int end = (p + 1 < numPackets) ? packets[p + 1] : count;

    String json = "{\"p\":";
    json += (p + 1);
    json += ",\"t\":";
    json += numPackets;
    json += ",\"cells\":[";
    bool first = true;
    for (int i = start; i < end; i++) {
      if (!first) json += ",";
      json += "{\"id\":";
      json += entries[i].id;
      if (entries[i].hotels > 0) { json += ",\"hotels\":"; json += entries[i].hotels; }
      if (entries[i].houses > 0) { json += ",\"houses\":"; json += entries[i].houses; }
      json += "}";
      first = false;
    }
    json += "]}";

    esp_err_t result = esp_now_send(centralMAC, (uint8_t*)json.c_str(), json.length());
    Serial.printf("[ESP-NOW] 发送 (%d/%d, %d bytes): ", p + 1, numPackets, json.length());
    Serial.println(json);
    if (result != ESP_OK) {
      Serial.printf("[ESP-NOW] Send error: %d\n", result);
    }
    if (p < numPackets - 1) delay(20);  // 包间间隔，避免拥塞
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
  uint32_t serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000) delay(100);

  // 初始化 Vision AI
  AI.begin();
  delay(200);

  const char cfg[] = "AT+TSCORE=40\r\n";
  AI.write(cfg, strlen(cfg));
  readResponse(1000);

  const char iou[] = "AT+TIOU=30\r\n";
  AI.write(iou, strlen(iou));
  readResponse(1000);

  // 初始化 ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);  // 锁定 channel 1

  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  Serial.printf("[ESP-NOW] Camera MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.printf("[ESP-NOW] Central MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                centralMAC[0], centralMAC[1], centralMAC[2],
                centralMAC[3], centralMAC[4], centralMAC[5]);
  uint8_t ch = 0;
  wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  Serial.printf("[ESP-NOW] WiFi channel: %d\n", ch);

  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init FAILED!");
    return;
  }
  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  // 添加 CrowPanel 为 peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, centralMAC, 6);
  peerInfo.channel = ESPNOW_CHANNEL;
  peerInfo.encrypt = false;

  esp_err_t peerErr = esp_now_add_peer(&peerInfo);
  if (peerErr != ESP_OK && peerErr != ESP_ERR_ESPNOW_EXIST) {
    Serial.printf("[ESP-NOW] Add peer FAILED: %d\n", peerErr);
  } else {
    espNowReady = true;
  }

  Serial.println("系统就绪，ESP-NOW 发送中...");
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
    sendBoardState();
  }

  delay(200);
}
