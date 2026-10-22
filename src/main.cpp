#include <Arduino.h>
#include "cn_font.h"

// 电脑性能监视器：电脑端 pc_monitor.py 每秒经 USB 发一行数据，ESP32 画到淘晶驰串口屏上
// 淘晶驰串口屏：屏 TX(黄) -> GPIO18，屏 RX(黑) -> GPIO17，屏 GND(红) 与 ESP32 共地
#define SCREEN_RX 18
#define SCREEN_TX 17
#define RUN_BAUD 921600  // 联机后把屏临时切到这个波特率，曲线整列重画也够快（屏重启后自动回到 9600）

// 颜色用 RGB565 十进制
#define C_BLACK   0
#define C_WHITE   65535
#define C_GRAY    33808   // 0x8410
#define C_DARK    8452    // 0x2104 进度条底色
#define C_HEADER  2145    // 0x0861 深蓝
#define C_GREEN   2016    // 0x07E0
#define C_YELLOW  65504   // 0xFFE0
#define C_RED     63488   // 0xF800
#define C_CYAN    2047    // 0x07FF
#define C_TEAL    1007    // 0x03EF 曲线下方填充
#define C_ORANGE  64800   // 0xFD20 上传曲线
#define C_DOT     46518   // 0xB5B6 小数点用浅灰，接近抗锯齿后的笔画亮度

// 屏里原工程自带的两个字库（实测字符集）：
//   1 号 24px：A-Z 0-9 / + 空格，没有小写、冒号、点、百分号
//   0 号 16px：0-9 % . 空格
// 冒号和小数点由 ESP32 画小方块补上
#define F_BIG   1
#define F_SMALL 0

// 布局（屏 320x240 横屏）
const int HEADER_H = 30;
const int ROW_Y[3] = {34, 62, 90};
const int ROW_H = 28;
const int LABEL_X = 4, LABEL_W = 48;
const int BAR_X = 54, BAR_W = 186, BAR_H = 18;
const int PCT_X = 244, PCT_W = 56;  // 百分比数字右对齐，后面跟小号 %
const int INFO_Y[3] = {120, 145, 170};
const int INFO_H = 25;
const int GRAPH_X = 4, GRAPH_Y = 198, GRAPH_W = 312, GRAPH_H = 40;
const int GRAPH_N = 60;  // 60 秒历史，每秒一列
// 时间三格 HH MM SS
const int TIME_X[3] = {206, 244, 282}, TIME_W = 30;

struct Metrics {
  float cpu = 0, ram = 0, gpu = -1, gpuTemp = -1;
  float ramUsed = 0, ramTotal = 0, vramUsed = 0, vramTotal = 0;  // GB
  float up = 0, down = 0;  // 字节/秒
  String time;
};

// 底部曲线可以切换显示的指标，点哪一行就显示哪一行
enum GraphMetric { G_CPU, G_RAM, G_GPU, G_NET, G_COUNT };
float history[G_COUNT][GRAPH_N];  // 最新的在末尾；G_NET 存下载 KB/s
float historyUp[GRAPH_N];          // 上传 KB/s，和下载画在同一张图
int historyLen = 0;
int graphMetric = G_CPU;
int drawnKey[GRAPH_N];  // 每列当前画在屏上的内容（高度编码），没变的列不重画

const uint8_t BRIGHTNESS[] = {100, 50, 20};
int brightnessIdx = 0;
bool screenReady = false;
uint32_t lastDataMs = 0;
bool showingNoData = false;

// 日志同时输出到原生 USB 口(Serial) 和 UART 口(Serial0)，插哪个 USB 口都能看到
void logf(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
  Serial0.print(buf);
}

// 流控：bkcmd=3 时屏每执行完一条指令回一个结果码，最多 FLOW_WINDOW 条在路上，
// 按屏的实际处理速度发，既不会把屏的串口缓冲区灌爆（24），也不白等
const int FLOW_WINDOW = 16;
bool flowControl = false;
int outstanding = 0;
bool touchPending = false;
int touchX, touchY;

void pumpScreen();

void sendCmd(const String &cmd) {
  if (flowControl) {
    uint32_t t = millis();
    while (outstanding >= FLOW_WINDOW) {
      pumpScreen();
      if (millis() - t > 200) {  // 屏没回（比如重启了），别卡死
        outstanding = 0;
        break;
      }
    }
    outstanding++;
  }
  Serial1.print(cmd);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
  Serial1.write(0xFF);
}

// 读取屏回复，返回可见字符（不可见字节用 . 代替）
String readReply(uint32_t waitMs) {
  String text;
  uint32_t start = millis();
  while (millis() - start < waitMs) {
    while (Serial1.available()) {
      uint8_t c = Serial1.read();
      text += (c >= 0x20 && c < 0x7F) ? (char)c : '.';
      start = millis();  // 还有数据就继续等
    }
    delay(2);
  }
  return text;
}

// 用淘晶驰的 connect 联机指令探测波特率，屏会回 "comok ..."
uint32_t detectBaud() {
  const uint32_t bauds[] = {9600, 115200, 19200, 38400, 57600, 230400, 256000, 512000, 921600, 4800, 2400};
  for (uint32_t baud : bauds) {
    Serial1.end();
    Serial1.begin(baud, SERIAL_8N1, SCREEN_RX, SCREEN_TX);
    delay(50);
    while (Serial1.available()) Serial1.read();
    sendCmd("DRAKJHSUYDGBNCJHGJKSHBDN");  // 官方联机前的清缓存垃圾指令
    sendCmd("connect");
    String reply = readReply(300);
    if (reply.indexOf("comok") >= 0) {
      logf("联机成功 %lu bps: %s\r\n", baud, reply.c_str());
      return baud;
    }
  }
  return 0;
}

// 在区域内画一行文字，sta=1 用背景色覆盖旧内容；xcen 0 左 1 中 2 右
void text(int font, int x, int y, int w, int h, uint16_t fg, uint16_t bg, int xcen, const String &s) {
  sendCmd(String("xstr ") + x + "," + y + "," + w + "," + h + "," + font + "," + fg + "," + bg + "," + xcen + ",1,1,\"" + s + "\"");
}

void fillRect(int x, int y, int w, int h, uint16_t c) {
  sendCmd(String("fill ") + x + "," + y + "," + w + "," + h + "," + c);
}

uint16_t levelColor(float pct) {
  if (pct < 60) return C_GREEN;
  if (pct < 85) return C_YELLOW;
  return C_RED;
}

// 大字库没有小写和点，网速用整数 K/M
String fmtSpeed(float bps) {
  float kb = bps / 1024;
  if (kb < 10000) return String((int)(kb + 0.5)) + "K";
  return String((int)(kb / 1024 + 0.5)) + "M";
}

// 一位小数：整数部分右对齐在 [xInt, xInt+wInt)，后面画小数点，再写小数位；返回结束的 x
int drawDecimal(int xInt, int wInt, int y, float v) {
  int tenths = (int)(v * 10 + 0.5);
  text(F_BIG, xInt, y, wInt, INFO_H, C_WHITE, C_BLACK, 2, String(tenths / 10));
  fillRect(xInt + wInt + 1, y + 19, 2, 2, C_DOT);
  int xFrac = xInt + wInt + 4;
  text(F_BIG, xFrac, y, 13, INFO_H, C_WHITE, C_BLACK, 0, String(tenths % 10));
  return xFrac + 13;
}

// 画汉字：屏里没有中文字库，按 cn_font.h 里的矩形逐块 fill，适合只画一次的固定标签
void drawCN(int x, int y, const char *s, uint16_t color) {
  while (*s) {
    uint8_t c = *s;
    int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    for (const CnGlyph &g : CN_GLYPHS) {
      if (strncmp(g.utf8, s, len) == 0 && g.utf8[len] == 0) {
        const uint8_t *r = CN_RECTS + g.offset * 4;
        for (int i = 0; i < g.count; i++, r += 4) fillRect(x + r[0], y + r[1], r[2], r[3], color);
        delay(3);  // 给屏一点时间处理，避免串口缓冲区溢出
        break;
      }
    }
    x += CN_CELL;
    s += len;
  }
}

void drawTimeColons() {
  for (int i = 0; i < 2; i++) {
    int x = TIME_X[i] + TIME_W + 3;
    fillRect(x, 11, 2, 2, C_DOT);
    fillRect(x, 19, 2, 2, C_DOT);
  }
}

void drawGraph();

// 曲线正在显示的指标，标签高亮成青色
void drawLabel(int metric) {
  uint16_t c = metric == graphMetric ? C_CYAN : C_GRAY;
  switch (metric) {
    case G_CPU: text(F_BIG, LABEL_X, ROW_Y[0], LABEL_W, ROW_H, c, C_BLACK, 0, "CPU"); break;
    case G_RAM: drawCN(LABEL_X, ROW_Y[1] + (ROW_H - CN_CELL) / 2, "内存", c); break;
    case G_GPU: drawCN(LABEL_X, ROW_Y[2] + (ROW_H - CN_CELL) / 2, "显卡", c); break;
    case G_NET: {  // 网速图同时画上传和下载，标签颜色就是图例
      int y = INFO_Y[2] + (INFO_H - CN_CELL) / 2;
      drawCN(LABEL_X, y, "上传", metric == graphMetric ? C_ORANGE : C_GRAY);
      drawCN(160, y, "下载", c);
      break;
    }
  }
}

void drawStatic() {
  sendCmd(String("cls ") + C_BLACK);
  fillRect(0, 0, 320, HEADER_H, C_HEADER);
  drawCN(6, 4, "电脑监控", C_WHITE);
  for (int i = 0; i < 3; i++) {
    fillRect(BAR_X, ROW_Y[i] + (ROW_H - BAR_H) / 2, BAR_W, BAR_H, C_DARK);
    text(F_SMALL, PCT_X + PCT_W + 1, ROW_Y[i], 18, ROW_H, C_GRAY, C_BLACK, 0, "%");
  }
  int cy = (INFO_H - CN_CELL) / 2;
  drawCN(LABEL_X, INFO_Y[0] + cy, "已用", C_GRAY);
  drawCN(LABEL_X, INFO_Y[1] + cy, "显存", C_GRAY);
  drawCN(204, INFO_Y[1] + cy, "温度", C_GRAY);
  drawCN(290, INFO_Y[1] + cy, "℃", C_DOT);
  for (int m = 0; m < G_COUNT; m++) drawLabel(m);
  sendCmd(String("draw ") + GRAPH_X + "," + GRAPH_Y + "," + (GRAPH_X + GRAPH_W - 1) + "," + (GRAPH_Y + GRAPH_H - 1) + "," + C_GRAY);
  for (int i = 0; i < GRAPH_N; i++) drawnKey[i] = -1;
  drawGraph();
}

void drawBar(int row, float pct) {
  int y = ROW_Y[row] + (ROW_H - BAR_H) / 2;
  if (pct < 0) {  // 没有这项数据
    fillRect(BAR_X, y, BAR_W, BAR_H, C_DARK);
    text(F_BIG, PCT_X, ROW_Y[row], PCT_W, ROW_H, C_GRAY, C_BLACK, 2, "N/A");
    return;
  }
  pct = constrain(pct, 0, 100);
  int w = (int)(BAR_W * pct / 100 + 0.5);
  // 先画已用部分再画剩余部分，不清整条，避免闪烁
  if (w > 0) fillRect(BAR_X, y, w, BAR_H, levelColor(pct));
  if (w < BAR_W) fillRect(BAR_X + w, y, BAR_W - w, BAR_H, C_DARK);
  text(F_BIG, PCT_X, ROW_Y[row], PCT_W, ROW_H, C_WHITE, C_BLACK, 2, String((int)(pct + 0.5)));
}

// 面积图：每列先算出从上到下每个像素的最终颜色，再按连续色段 fill，
// 不先清屏、不重叠覆盖，所以滚动时不闪；内容没变的列跳过。
// 网速图：下载画成面积，上传画成橙色折线（和左边一列的高度连起来）
void drawGraph() {
  const int ix = GRAPH_X + 1, iy = GRAPH_Y + 1, iw = GRAPH_W - 2, ih = GRAPH_H - 2;
  bool net = graphMetric == G_NET;
  float scale = 100;
  if (net) {  // 网速按窗口内上传下载的最大值自动缩放，最小 64KB/s 满格
    scale = 64;
    for (int i = 0; i < historyLen; i++) scale = max(scale, max(history[G_NET][i], historyUp[i]));
  }
  auto heightOf = [&](float v) { return constrain((int)(v / scale * ih + 0.5), 0, ih); };

  int prevUp = -1;
  for (int col = 0; col < GRAPH_N; col++) {
    int idx = col - (GRAPH_N - historyLen);  // 历史不满时左边留空
    int h = idx >= 0 ? heightOf(history[graphMetric][idx]) : 0;
    int lo = -1, hi = -1;  // 上传折线占的像素行（从底部数，0 是最下面一行）
    if (net && idx >= 0) {
      int k = min(heightOf(historyUp[idx]), ih - 1);
      int p = prevUp < 0 ? k : prevUp;
      lo = max(0, min(p, k) - 1);
      hi = max(p, k);
      prevUp = k;
    }
    int key = h | (lo + 1) << 8 | (hi + 1) << 16;
    if (key == drawnKey[col]) continue;
    drawnKey[col] = key;

    int x0 = ix + col * iw / GRAPH_N, w = ix + (col + 1) * iw / GRAPH_N - x0;
    int runStart = 0;
    uint16_t runColor = 0;
    for (int r = 0; r <= ih; r++) {
      uint16_t c = 0;
      if (r < ih) {
        int k = ih - 1 - r;
        c = k >= lo && k <= hi ? C_ORANGE : k < h ? (k >= h - 2 ? C_CYAN : C_TEAL) : C_BLACK;
      }
      if (r == 0) runColor = c;
      else if (r == ih || c != runColor) {
        fillRect(x0, iy + runStart, w, r - runStart, runColor);
        runStart = r;
        runColor = c;
      }
    }
  }
}

void drawMetrics(const Metrics &m) {
  // 时间 "12:34:56" 拆成三格，冒号自己画
  if (m.time.length() == 8) {
    for (int i = 0; i < 3; i++)
      text(F_BIG, TIME_X[i], 3, TIME_W, 24, C_WHITE, C_HEADER, 1, m.time.substring(i * 3, i * 3 + 2));
    drawTimeColons();
  }
  drawBar(0, m.cpu);
  drawBar(1, m.ram);
  drawBar(2, m.gpu);

  // RAM 49.4/128G
  int x = drawDecimal(56, 44, INFO_Y[0], m.ramUsed);
  text(F_BIG, x, INFO_Y[0], 120, INFO_H, C_WHITE, C_BLACK, 0, "/" + String((int)(m.ramTotal + 0.5)) + "G");
  // VRAM 2.3/8G   TEMP 42C
  if (m.gpuTemp >= 0) {
    x = drawDecimal(68, 26, INFO_Y[1], m.vramUsed);
    text(F_BIG, x, INFO_Y[1], 88, INFO_H, C_WHITE, C_BLACK, 0, "/" + String((int)(m.vramTotal + 0.5)) + "G");
    text(F_BIG, 252, INFO_Y[1], 40, INFO_H, C_WHITE, C_BLACK, 2, String((int)(m.gpuTemp + 0.5)));
  }
  // UP 312K   DN 1250K
  text(F_BIG, 56, INFO_Y[2], 100, INFO_H, C_WHITE, C_BLACK, 0, fmtSpeed(m.up));
  text(F_BIG, 210, INFO_Y[2], 106, INFO_H, C_WHITE, C_BLACK, 0, fmtSpeed(m.down));

  if (historyLen == GRAPH_N) {
    for (int g = 0; g < G_COUNT; g++) memmove(history[g], history[g] + 1, (GRAPH_N - 1) * sizeof(float));
    memmove(historyUp, historyUp + 1, (GRAPH_N - 1) * sizeof(float));
    historyLen--;
  }
  history[G_CPU][historyLen] = m.cpu;
  history[G_RAM][historyLen] = m.ram;
  history[G_GPU][historyLen] = max(m.gpu, 0.0f);
  history[G_NET][historyLen] = m.down / 1024;
  historyUp[historyLen] = m.up / 1024;
  historyLen++;
  drawGraph();
}

// 解析 "M cpu=12.3 ram=45.6 ... time=12:34:56"
bool parseMetrics(const String &line, Metrics &m) {
  if (!line.startsWith("M ")) return false;
  int pos = 2;
  while (pos < (int)line.length()) {
    int sp = line.indexOf(' ', pos);
    if (sp < 0) sp = line.length();
    String kv = line.substring(pos, sp);
    pos = sp + 1;
    int eq = kv.indexOf('=');
    if (eq < 0) continue;
    String k = kv.substring(0, eq), v = kv.substring(eq + 1);
    if (k == "cpu") m.cpu = v.toFloat();
    else if (k == "ram") m.ram = v.toFloat();
    else if (k == "ramu") m.ramUsed = v.toFloat();
    else if (k == "ramt") m.ramTotal = v.toFloat();
    else if (k == "gpu") m.gpu = v.toFloat();
    else if (k == "gput") m.gpuTemp = v.toFloat();
    else if (k == "vramu") m.vramUsed = v.toFloat();
    else if (k == "vramt") m.vramTotal = v.toFloat();
    else if (k == "up") m.up = v.toFloat();
    else if (k == "down") m.down = v.toFloat();
    else if (k == "time") m.time = v;
  }
  return true;
}

bool screenInit() {
  flowControl = false;
  uint32_t baud = detectBaud();
  if (baud == 0) return false;
  if (baud != RUN_BAUD) {
    sendCmd(String("baud=") + RUN_BAUD);  // baud 是临时设置，不会写进屏，掉电恢复
    Serial1.flush();
    delay(100);
    Serial1.updateBaudRate(RUN_BAUD);
  }
  sendCmd("bkcmd=3");    // 每条指令都回结果码，用来做流控
  delay(50);
  while (Serial1.available()) Serial1.read();
  outstanding = 0;
  flowControl = true;
  sendCmd("tsw 255,0");  // 关掉屏里原工程控件的触摸，避免它自己跳页把画面刷掉
  sendCmd("thsp=0");     // 不自动睡眠
  sendCmd("sendxy=1");   // 触摸时主动上报坐标：67 xH xL yH yL 状态 FF FF FF
  sendCmd(String("dim=") + BRIGHTNESS[brightnessIdx]);
  drawStatic();
  showingNoData = false;
  logf("界面已绘制，等待电脑数据\r\n");
  return true;
}

void selectGraph(int metric) {
  if (metric == graphMetric) return;
  int old = graphMetric;
  graphMetric = metric;
  drawLabel(old);
  drawLabel(metric);
  for (int i = 0; i < GRAPH_N; i++) drawnKey[i] = -1;
  drawGraph();
}

// 点标题栏切亮度；点 CPU/内存/显卡 行或上传下载那一行切曲线；点曲线轮流切
void handleTouch(int x, int y) {
  if (y < HEADER_H) {
    brightnessIdx = (brightnessIdx + 1) % sizeof(BRIGHTNESS);
    sendCmd(String("dim=") + BRIGHTNESS[brightnessIdx]);
    return;
  }
  for (int r = 0; r < 3; r++)
    if (y >= ROW_Y[r] && y < ROW_Y[r] + ROW_H) return selectGraph(r);
  if (y >= INFO_Y[2] && y < INFO_Y[2] + INFO_H) return selectGraph(G_NET);
  if (y >= GRAPH_Y) selectGraph((graphMetric + 1) % G_COUNT);
}

void handleFrame(const uint8_t *f, size_t len) {
  if (len == 1 && f[0] <= 0x24) {  // 指令执行结果码
    if (outstanding > 0) outstanding--;
    if (f[0] != 0x01) logf("屏返回错误码: %02X\r\n", f[0]);  // 24 = 串口缓冲区溢出
    return;
  }
  if (f[0] == 0x67 && len == 6) {
    if (f[5] == 1) {  // 只处理按下；发指令等确认时也会收到触摸，先记下来回到 loop 再处理
      touchX = (f[1] << 8) | f[2];
      touchY = (f[3] << 8) | f[4];
      touchPending = true;
    }
    return;
  }
  if (f[0] == 0x88) {
    logf("屏重启了，重新初始化\r\n");
    screenReady = false;
    flowControl = false;
    return;
  }
  String hex;
  for (size_t i = 0; i < len; i++) {
    char b[4];
    snprintf(b, sizeof(b), "%02X ", f[i]);
    hex += b;
  }
  logf("屏返回: %s\r\n", hex.c_str());
}

// 按 FF FF FF 结尾切帧
void pumpScreen() {
  static uint8_t buf[64];
  static size_t len = 0;
  while (Serial1.available()) {
    uint8_t c = Serial1.read();
    if (len < sizeof(buf)) buf[len++] = c;
    else len = 0;
    if (len >= 4 && buf[len - 1] == 0xFF && buf[len - 2] == 0xFF && buf[len - 3] == 0xFF) {
      handleFrame(buf, len - 3);
      len = 0;
    }
  }
}

// USB 收到的行：以 "M " 开头是监控数据，其它当作屏指令直接转发（调试用）
void pollHost(Stream &in, String &line) {
  while (in.available()) {
    char c = in.read();
    if (c != '\r' && c != '\n') {
      if (line.length() < 300) line += c;
      continue;
    }
    if (!line.length()) continue;
    Metrics m;
    if (line.startsWith("G ")) {  // 调试：电脑发 "G n" 直接切曲线，不用手点
      if (screenReady) selectGraph(line.substring(2).toInt() % G_COUNT);
    } else if (parseMetrics(line, m)) {
      if (screenReady) {
        if (showingNoData) fillRect(196, 3, 120, 24, C_HEADER);  // 擦掉“等待数据”
        drawMetrics(m);
      }
      lastDataMs = millis();
      showingNoData = false;
    } else if (line[0] != 'M') {
      sendCmd(line);
      logf("发送: %s\r\n", line.c_str());
    }
    line = "";
  }
}

void setup() {
  Serial.setRxBufferSize(4096);  // 画屏期间电脑的数据先攒着，别丢
  Serial.begin(115200);
  Serial0.begin(115200);
  delay(1500);
  logf("\r\n=== ESP32 电脑性能监视器 ===\r\n");
}

void loop() {
  if (!screenReady) {
    screenReady = screenInit();
    if (!screenReady) {
      logf("联机失败，2 秒后重试（检查 TX/RX 和共地）\r\n");
      delay(2000);
    }
    return;
  }
  static String usbLine, uartLine;
  pollHost(Serial, usbLine);
  pollHost(Serial0, uartLine);
  pumpScreen();
  if (touchPending) {
    touchPending = false;
    handleTouch(touchX, touchY);
  }

  // 3 秒没收到电脑数据，在标题栏提示
  if (!showingNoData && millis() - lastDataMs > 3000) {
    fillRect(196, 3, 120, 24, C_HEADER);
    drawCN(208, 4, "等待数据", C_RED);
    showingNoData = true;
  }
}
