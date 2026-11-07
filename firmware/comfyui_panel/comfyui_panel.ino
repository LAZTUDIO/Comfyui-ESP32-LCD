/* ComfyUI LAN monitor panel (v4 structure)
 * Board: Waveshare ESP32-S3-Touch-LCD-3.5 (ST7796S SPI 80MHz, FT6336U, TCA9554)
 *
 * Pages per host: [MONITOR] [OUTPUTS] — swipe through 2*nHosts pages.
 *  MONITOR: state / elapsed / node / steps / queue, VRAM & RAM, scrolling run log
 *  OUTPUTS: 4 recent outputs with best-effort thumbnails (ComfyUI /view preview)
 * Worker task (core 0): HTTP polling, WebSocket, log events, JPEG thumbs.
 * UI task (core 1): LVGL only; pulls changed snapshots; updates only on change.
 */
#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include "TCA9554.h"
#include "TouchDrvFT6X36.hpp"
#include <lvgl.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#include <JPEGDEC.h>
#include "cn_fonts.h"
#include "gpu_logos.h"

// ---------- pins ----------
#define LCD_SCK 5
#define LCD_MOSI 1
#define LCD_MISO 2
#define LCD_DC 3
#define LCD_BL 6
#define I2C_SDA 8
#define I2C_SCL 7
#define UI_W 480
#define UI_H 320
#define GFX_ROT 1
#define TOUCH_MAP 0
#define DEFAULT_PORT 8188
#define MAX_HOSTS 4
#define MAX_JOBS 4
#define LOG_LINES 10
#define LCD_SPI_HZ 80000000
#define THUMB_W 100
#define THUMB_H 84
#define THUMB_JPEG_MAX (384 * 1024)
#define OFFLINE_MS 10000  // no successful API response for this long => offline

TCA9554 TCA(0x20);
TouchDrvFT6X36 touch;
Arduino_DataBus *bus = new Arduino_ESP32SPI(LCD_DC, -1, LCD_SCK, LCD_MOSI, LCD_MISO);
Arduino_GFX *gfx = new Arduino_ST7796(bus, -1, 0, true, 320, 480);

lv_color_t *disp_buf1, *disp_buf2;
uint32_t bufSize;
uint16_t *fbMirror = nullptr;
WebServer shotSrv(8080);
bool shotSrvStarted = false;

// ---------- config ----------
Preferences prefs;
String cfgSsid, cfgPsk;
String hostStr[MAX_HOSTS];
int hostPort[MAX_HOSTS];
int nHosts = 0;
bool ultraEnabled = false;      // Ultra overview fed by the on-host assistant
int ultraPort = 8189;           // assistant port A (same on every host)
volatile int activeHost = 0;
volatile int activePage = 0;  // 0 monitor, 1 outputs
volatile int activePageIdx = 0;

enum WifiSt { W_NONE, W_CONNECTING, W_ONLINE };
volatile WifiSt wst = W_NONE;
uint32_t wifiTrySince = 0;

// ---------- data ----------
struct Vitals {
  bool ok = false;
  double ram_total = 0, ram_free = 0;
  double vram_total = 0, vram_free = 0;
  char gpu_name[48] = "";
  char gpu[24] = "";        // short model, e.g. "RTX 5070" / "RX 7900 XT"
  uint8_t gpuVendor = 0;    // 0 unknown, 1 NVIDIA, 2 AMD
  char version[12] = "";
  char accel[20] = "";    // "ROCm 10.1.0" / "CUDA 12.8" ... from the pytorch build tag
};
struct Run {
  bool connected = false;
  bool running = false;
  int pending = 0;
  char node[16] = "";
  float pv = 0, pmax = 0, stepSec = 0;
  uint32_t start_ms = 0;
};
// Ultra: hardware vitals from the on-host "看板助手" (/api/panel on port A).
// Percent fields are -1 when unknown; watts < 0 when unreadable.
struct Ultra {
  bool ok = false;          // assistant answered recently
  bool everSeen = false;
  uint32_t lastSeen = 0;
  int cpu = -1, gpu = -1, ram = -1, vram = -1;
  float watts = -1;
  float gpuTemp = -1, cpuTemp = -1;
  char gpuName[48] = "";
  uint8_t gpuVendor = 0;    // 0 unknown, 1 NVIDIA, 2 AMD
};
struct Job {
  char file[56] = "";
  char subfolder[32] = "";
  char type[10] = "output";
  char poster[56] = "";
  uint32_t dur_ms = 0;
  uint32_t end_epoch = 0;
  uint16_t width = 0, height = 0, frames = 0;
  float fps = 0, videoSec = 0;
  uint32_t sizeBytes = 0;
  bool sizeKnown = false;
  uint32_t sizeAt = 0;
  bool ok = true;
};
struct NetData {
  char host[40] = "";
  int port = DEFAULT_PORT;
  Vitals vitals;
  Run run;
  Ultra ultra;
  Job jobs[MAX_JOBS];
  int nJobs = 0;
  bool serverOk = false;
  bool everSeen = false;    // got at least one successful response since boot
  uint32_t lastSeen = 0;    // millis of last successful API response
  uint32_t ver = 0;
  char logLines[LOG_LINES][52] = {{0}};
  int logCount = 0;
  uint32_t logVer = 0;
  uint16_t *thumbs[MAX_JOBS] = {nullptr};
  uint32_t thumbVer[MAX_JOBS] = {0};
  uint8_t thumbState[MAX_JOBS] = {0};  // 0 none, 1 ok, 2 failed, 3 unsupported
  uint32_t thumbAt[MAX_JOBS] = {0};
  char thumbSig[MAX_JOBS][80] = {{0}};
  uint8_t thumbScan = 0;
  uint32_t lastThumbTry = 0;
  uint32_t lastSizeTry = 0;
  bool wantThumbs = false;
  char promptClient[40] = "";
  int nodeTotal = 0, nodesDone = 0, nodesCached = 0;
  uint32_t nodeSeen[64];
  int nodeSeenCount = 0;
  float lastStepVal = -1;
  uint32_t lastStepMs = 0;
  uint32_t lastStepLog = 0;
  uint32_t lastHeartbeat = 0;
  uint32_t tV = 0, tQ = 0, tH = 0, tU = 0;
};
NetData net[MAX_HOSTS];
SemaphoreHandle_t netMux = nullptr;
volatile uint32_t netEpoch = 0;
volatile int netActive = 0;
JPEGDEC jpeg;
uint16_t *scrThumb = nullptr;   // worker 100x84 decode scratch
uint8_t *thumbJpg = nullptr;

// ---------- UI-side copies ----------
Vitals vitals;
Run run;
Ultra ultra;
Job jobs[MAX_JOBS];
int nJobs = 0;
bool serverOk = false, everSeen = false;
int nodeTotal = 0, nodesDone = 0, nodesCached = 0;
struct HostView {
  Vitals vitals; Run run; Ultra ultra; Job jobs[MAX_JOBS]; int nJobs = 0;
  bool serverOk = false, everSeen = false;
  int nodeTotal = 0, nodesDone = 0, nodesCached = 0;
};

uint32_t lastVer[MAX_HOSTS] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
uint32_t lastLogVer[MAX_HOSTS] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
uint32_t lastThumbVer[MAX_HOSTS][MAX_JOBS] = {{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                              {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                              {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                              {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}};

uint32_t uiMaxUs = 0;

// ---------- UI themes ----------
struct Theme {
  const char *name;
  uint32_t bg, head, card, inner, accent, ok, warn, err, txt, mut, line;
  uint8_t radius;
  bool light;
};
static const char *THEME_ZH[10] = {"深空玻璃","极简白","赛博霓虹","复古终端","工业橙","蒸汽波","纸墨","蓝图","材质 You","极光"};
static const Theme THEMES[10] = {
  {"Dark Glass",   0x0d1220,0x111a2e,0x18203a,0x101828,0x4f8cff,0x2ecc71,0xf5a623,0xe5484d,0xdfe7f5,0x8494ad,0x223154, 8,false},
  {"Minimal Light",0xf4f5f7,0xffffff,0xffffff,0xf8f9fb,0x151a22,0x16a34a,0xd97706,0xdc2626,0x151a22,0x7b8496,0xdfe3ea,14,true},
  {"Cyber Neon",   0x08060f,0x0c0818,0x120c22,0x180e2c,0x00f0ff,0xff2d95,0xffd60a,0xff3b30,0xf5f0ff,0x9682be,0x3d1230, 2,false},
  {"Terminal",     0x020a04,0x020a04,0x021406,0x021406,0x39ff14,0x39ff14,0xa6ff00,0xff4d4d,0x39ff14,0x1e8c28,0x14501e, 0,false},
  {"Industrial",   0x1b1f24,0x14181c,0x262c33,0x1e2328,0xff9f1c,0x33cccc,0xff9f1c,0xff5757,0xe8edf2,0x96a0aa,0x3a424b, 2,false},
  {"Synthwave",    0x2b1055,0x1a0c2c,0x2e1656,0x241244,0xff4ecd,0x00e5ff,0xffd166,0xff5c8a,0xfff0ff,0xbea0de,0x6a3a9a, 4,false},
  {"Paper Ink",    0xf6f1e7,0xf6f1e7,0xfffcf6,0xf9f4ea,0xb23a2f,0x3a6e48,0xb26a00,0xa51f1f,0x2b2620,0x7a6e5e,0xd8cfc0, 3,true},
  {"Blueprint",    0x0a1f3c,0x0a1f3c,0x0c284c,0x0e2f58,0x5fd4ff,0x78ffd6,0xffd166,0xff6b6b,0xd7f2ff,0x8cbedc,0x285a8c, 0,false},
  {"Material You", 0xf3edf7,0xf3edf7,0xffffff,0xe8def8,0x6750a4,0x388e3c,0xb26a00,0xb3261e,0x1d1b20,0x79747e,0xe7e0ec,16,true},
  {"Aurora",       0x071a1f,0x0a2029,0x0e2a33,0x0c242d,0x2de2c4,0x2de2c4,0xffb347,0xff6b6b,0xe6fbf6,0x8cbeb9,0x1e5055,10,false},
};
int themeIdx = 0;
int lang = 0;  // 0 EN, 1 ZH
// The page layout follows the number of configured servers:
// 1 -> detail + outputs, 2 -> 2 columns, 3 -> 3 columns, 4 -> 2x2 grid.
static inline bool useGrid() { return nHosts >= 2; }
static inline int gridCols() { return nHosts >= 4 ? 2 : (nHosts < 2 ? 1 : nHosts); }
static inline int gridRows() { return nHosts >= 4 ? 2 : 1; }
static inline int pageCount() { return useGrid() ? 3 : 2; }
static inline int cellJobs() { return nHosts == 2 ? 2 : 1; }
static inline uint32_t pollVitalsMs() { return nHosts >= 4 ? 5000 : (nHosts == 3 ? 4000 : 3000); }
static inline uint32_t pollQueueMs() { return nHosts >= 4 ? 3500 : (nHosts == 3 ? 3000 : 2000); }
static inline uint32_t offlineMs() { return nHosts >= 4 ? 20000 : (nHosts == 3 ? 15000 : OFFLINE_MS); }
Theme TH;
#define L(en, zh) (lang ? (zh) : (en))
#define C_BG lv_color_hex(TH.bg)
#define C_HEAD lv_color_hex(TH.head)
#define C_CARD lv_color_hex(TH.card)
#define C_INNER lv_color_hex(TH.inner)
#define C_ACCENT lv_color_hex(TH.accent)
#define C_OK lv_color_hex(TH.ok)
#define C_WARN lv_color_hex(TH.warn)
#define C_ERR lv_color_hex(TH.err)
#define C_TXT lv_color_hex(TH.txt)
#define C_MUT lv_color_hex(TH.mut)
#define C_LINE lv_color_hex(TH.line)
#define C_RAD TH.radius

lv_obj_t *scrSetup, *scrDash;
lv_obj_t *wifiPanel, *srvPanel, *themePanel, *apList, *lblWifiState, *lblSrvVal[MAX_HOSTS], *lblScanStatus;
lv_obj_t *btnUltraToggle, *lblUltraToggle, *btnUltraPort, *lblUltraPort;
lv_obj_t *editOverlay, *editTitle, *editTa, *editKb;
int editTarget = -1;
String pendingSsid;
String setupMsg;
lv_obj_t *lblHost, *lblClock, *dotServer, *pageDots[6], *lblPageSub, *tileView;
lv_obj_t *wifiBars[4] = {nullptr, nullptr, nullptr, nullptr};
lv_obj_t *tabBtn[3] = {nullptr, nullptr, nullptr};

struct MonUI {
  lv_obj_t *lblState = nullptr, *lblDur = nullptr, *lblNode = nullptr, *lblQueue = nullptr;
  lv_obj_t *barProg = nullptr, *lblStep = nullptr, *spinner = nullptr, *lblHint = nullptr;
  lv_obj_t *gBar[2] = {nullptr, nullptr}, *gVal[2] = {nullptr, nullptr};
  lv_obj_t *lblGpu = nullptr, *imgGpu = nullptr, *lblAcc = nullptr, *lblVer = nullptr;
  lv_obj_t *logLabel = nullptr;
  lv_obj_t *uLbl[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *uVal[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *uBar[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *uPower = nullptr;
};
struct OutUI {
  lv_obj_t *canvas[MAX_JOBS] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *name[MAX_JOBS] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *meta[MAX_JOBS] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *ph[MAX_JOBS] = {nullptr, nullptr, nullptr, nullptr};
};
MonUI mui[MAX_HOSTS];
OutUI oui[MAX_HOSTS];

HostView dview[MAX_HOSTS];
uint32_t dviewVer[MAX_HOSTS] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
uint32_t dviewLogVer[MAX_HOSTS] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
uint32_t dviewThumbVer[MAX_HOSTS][MAX_JOBS] = {{0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                               {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                               {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu},
                                               {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}};
// widgets of the auto grid (used for 2/3/4 hosts)
struct DashCell {
  lv_obj_t *title = nullptr, *state = nullptr, *dur = nullptr, *node = nullptr, *step = nullptr, *bar = nullptr;
  lv_obj_t *vramVal = nullptr, *vbar = nullptr, *ramVal = nullptr, *rbar = nullptr;
  lv_obj_t *gpu = nullptr, *img = nullptr, *acc = nullptr, *ver = nullptr, *queue = nullptr;
};
struct LogCell {
  lv_obj_t *title = nullptr, *state = nullptr, *log = nullptr;
  int lines = 0;
};
struct OutCell {
  lv_obj_t *title = nullptr;
  lv_obj_t *cv[MAX_JOBS] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *name[MAX_JOBS] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *meta[MAX_JOBS] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *ph[MAX_JOBS] = {nullptr, nullptr, nullptr, nullptr};
  int jobs = 1;
};
DashCell gdash[MAX_HOSTS];
LogCell glog[MAX_HOSTS];
OutCell gout[MAX_HOSTS];

// Ultra overview cell: base run info + CPU/GPU/RAM/VRAM utilization + power.
struct UltraCell {
  lv_obj_t *title = nullptr, *state = nullptr, *dur = nullptr, *node = nullptr,
           *step = nullptr, *bar = nullptr, *gpu = nullptr, *img = nullptr,
           *power = nullptr, *queue = nullptr;
  lv_obj_t *mLbl[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *mVal[4] = {nullptr, nullptr, nullptr, nullptr};
  lv_obj_t *mBar[4] = {nullptr, nullptr, nullptr, nullptr};
  bool compact = false;
};
UltraCell ucell[MAX_HOSTS];

// ================= helpers =================
static void setLbl(lv_obj_t *l, const char *txt) {
  if (!l) return;
  const char *cur = lv_label_get_text(l);
  if (!cur || strcmp(cur, txt)) lv_label_set_text(l, txt);
}
static void setBar(lv_obj_t *bar, int v, lv_color_t col, lv_obj_t *val, const char *txt) {
  if (bar) {
    if (v < 0) v = 0; if (v > 100) v = 100;
    if (lv_bar_get_value(bar) != v) lv_bar_set_value(bar, v, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, col, LV_PART_INDICATOR);
  }
  setLbl(val, txt);
}
static void setGpuMark(lv_obj_t *img, uint8_t vendor) {
  if (!img) return;
  const void *src = vendor == 1 ? (const void *)&GPU_LOGO_NVIDIA : (vendor == 2 ? (const void *)&GPU_LOGO_AMD : nullptr);
  lv_img_set_src(img, src);
}
static lv_obj_t *mkCard(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t bg = C_CARD) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, w, h);
  lv_obj_set_style_bg_color(c, bg, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, C_RAD, 0);
  lv_obj_set_style_pad_all(c, 6, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}
static lv_obj_t *mkLabel(lv_obj_t *parent, const char *txt, lv_color_t col, int x, int y, int font = 14) {
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_color(l, col, 0);
  if (font == 12) lv_obj_set_style_text_font(l, &cn_font_12, 0);
  else if (font == 14) lv_obj_set_style_text_font(l, &cn_font_14, 0);
  else if (font == 16) lv_obj_set_style_text_font(l, &cn_font_16, 0);
  else if (font == 20) lv_obj_set_style_text_font(l, &cn_font_20, 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

// ================= display =================
void lcd_reset() {
  TCA.write1(1, 1); delay(10);
  TCA.write1(1, 0); delay(120);
  TCA.write1(1, 1); delay(200);
}
void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif
  if (fbMirror) {
    for (uint32_t r = 0; r < h; r++) {
      uint32_t y = area->y1 + r;
      if (y >= UI_H) break;
      memcpy(&fbMirror[y * UI_W + area->x1], &color_p[r * w], w * 2);
    }
  }
  lv_disp_flush_ready(drv);
}
void my_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int16_t x[1], y[1];
  uint8_t t = touch.getPoint(x, y, 1);
  if (t) {
    int px = x[0], py = y[0];
    int lx = py, ly = 319 - px;
    if (lx < 0) lx = 0; if (lx >= UI_W) lx = UI_W - 1;
    if (ly < 0) ly = 0; if (ly >= UI_H) ly = UI_H - 1;
    data->point.x = lx;
    data->point.y = ly;
    data->state = LV_INDEV_STATE_PR;
  } else data->state = LV_INDEV_STATE_REL;
}
void handleShot() {
  if (!fbMirror) { shotSrv.send(500, "text/plain", "no fb"); return; }
  WiFiClient &c = shotSrv.client();
  const size_t total = UI_W * UI_H * 2;
  c.print("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: ");
  c.print((unsigned)total);
  c.print("\r\nConnection: close\r\n\r\n");
  const uint8_t *p = (const uint8_t *)fbMirror;
  size_t left = total;
  while (left) {
    size_t n = c.write(p, left > 4096 ? 4096 : left);
    if (!n) break;
    p += n; left -= n;
  }
}

// ================= locks / logs =================
static inline void lockNet() { if (netMux) xSemaphoreTake(netMux, portMAX_DELAY); }
static inline void unlockNet() { if (netMux) xSemaphoreGive(netMux); }
static void bump(NetData &nd) { nd.ver++; }
static void markSeenLocked(NetData &nd) { nd.serverOk = true; nd.everSeen = true; nd.lastSeen = millis(); }
// goes online->offline once no successful response arrived for OFFLINE_MS; caller holds lock
static bool markOfflineLocked(NetData &nd, uint32_t now) {
  if (!nd.serverOk || now - nd.lastSeen <= offlineMs()) return false;
  nd.serverOk = false;
  nd.vitals = Vitals();
  nd.run = Run();
  nd.nodeTotal = nd.nodesDone = nd.nodesCached = 0;
  nd.promptClient[0] = 0;
  bump(nd);
  return true;
}
// never answered yet: stay "connecting" for a grace period, then call it offline
static inline bool stillLinking(bool seen) { return !seen && millis() < 15000; }
static void addLogLocked(NetData &nd, const char *msg) {
  char line[52];
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(line, sizeof(line), "%02d:%02d:%02d %s", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, msg);
  } else {
    uint32_t s = millis() / 1000;
    snprintf(line, sizeof(line), "%02u:%02u:%02u %s", s / 3600, (s / 60) % 60, s % 60, msg);
  }
  if (nd.logCount == LOG_LINES) {
    memmove(nd.logLines[0], nd.logLines[1], sizeof(nd.logLines[0]) * (LOG_LINES - 1));
    nd.logCount--;
  }
  strlcpy(nd.logLines[nd.logCount], line, sizeof(nd.logLines[0]));
  nd.logCount++;
  nd.logVer++;
}
static void logEvent(NetData &nd, const char *fmt, ...) {
  char msg[44];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  lockNet();
  addLogLocked(nd, msg);
  unlockNet();
}

// ================= config =================
static void parseHost(int i, const String &s) {
  hostStr[i] = s;
  int c = s.indexOf(':');
  hostPort[i] = DEFAULT_PORT;
  if (c > 0) {
    hostStr[i] = s.substring(0, c);
    int p = s.substring(c + 1).toInt();
    if (p > 0) hostPort[i] = p;
  }
  hostStr[i].trim();
}
void loadCfg() {
  prefs.begin("cfg", true);
  cfgSsid = prefs.getString("ssid", "");
  cfgPsk = prefs.getString("psk", "");
  nHosts = 0;
  for (int i = 0; i < MAX_HOSTS; i++) {
    String v = prefs.getString(("h" + String(i)).c_str(), "");
    if (v.length()) { parseHost(nHosts, v); nHosts++; }
  }
  if (nHosts == 0) {
    String legacy = prefs.getString("host", "");
    int lp = prefs.getInt("port", DEFAULT_PORT);
    if (legacy.length()) { parseHost(0, legacy + ":" + String(lp)); nHosts = 1; }
  }
  themeIdx = prefs.getInt("theme", 0);
  lang = prefs.getInt("lang", 0);
  if (lang < 0 || lang > 1) lang = 0;
  ultraEnabled = prefs.getBool("ultra", false);
  ultraPort = prefs.getInt("ultraPort", 8189);
  if (ultraPort <= 0 || ultraPort > 65535) ultraPort = 8189;
  prefs.end();
  if (themeIdx < 0 || themeIdx > 9) themeIdx = 0;
  Serial.printf("cfg ssid='%s' hosts=%d theme=%d layout=%dx%d", cfgSsid.c_str(), nHosts, themeIdx, gridCols(), gridRows());
  for (int i = 0; i < nHosts; i++) Serial.printf(" [%s:%d]", hostStr[i].c_str(), hostPort[i]);
  Serial.println();
}
void saveCfg() {
  prefs.begin("cfg", false);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("psk", cfgPsk);
  for (int i = 0; i < MAX_HOSTS; i++) {
    String k = "h" + String(i);
    if (i < nHosts && hostStr[i].length()) prefs.putString(k.c_str(), hostStr[i] + ":" + String(hostPort[i]));
    else prefs.remove(k.c_str());
  }
  prefs.remove("host");
  prefs.remove("port");
  prefs.remove("dual");
  prefs.putBool("ultra", ultraEnabled);
  prefs.putInt("ultraPort", ultraPort);
  prefs.end();
}
void netConfigure() {
  lockNet();
  for (int i = 0; i < MAX_HOSTS; i++) {
    strlcpy(net[i].host, hostStr[i].c_str(), sizeof(net[i].host));
    net[i].port = hostPort[i];
    for (int k = 0; k < MAX_JOBS; k++)
      if (!net[i].thumbs[k]) net[i].thumbs[k] = (uint16_t *)heap_caps_malloc(THUMB_W * THUMB_H * 2, MALLOC_CAP_SPIRAM);
  }
  netActive = activeHost;
  netEpoch++;
  unlockNet();
}

// Persist a changed host list, rebuild the dashboard for the new host count and
// stay on it (the layout follows nHosts).
static void applyHostsAndShow() {
  saveCfg();
  netConfigure();
  if (nHosts > 0 && wst == W_ONLINE) {
    activeHost = 0; activePage = 0; activePageIdx = 0;
    buildDashboard();
    if (scrDash) lv_scr_load(scrDash);
    netEpoch++;
  } else if (scrSetup) showSetupTab(1);
}

// ================= setup UI =================
static void tabStyle(int active) {
  for (int i = 0; i < 3; i++) {
    if (!tabBtn[i]) continue;
    bool a = (i == active);
    lv_obj_set_style_bg_color(tabBtn[i], a ? C_ACCENT : C_CARD, 0);
    lv_obj_t *lab = lv_obj_get_child(tabBtn[i], 0);
    if (lab) {
      // contrast on accent background
      uint8_t r = (TH.accent >> 16) & 0xFF, g = (TH.accent >> 8) & 0xFF, b = TH.accent & 0xFF;
      bool bright = (r * 299 + g * 587 + b * 114) / 1000 > 160;
      lv_obj_set_style_text_color(lab, a ? (bright ? lv_color_black() : lv_color_white()) : C_TXT, 0);
    }
  }
}

void showSetupTab(int tab) {
  if (!wifiPanel) return;
  tabStyle(tab);
  lv_obj_add_flag(wifiPanel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(srvPanel, LV_OBJ_FLAG_HIDDEN);
  if (themePanel) lv_obj_add_flag(themePanel, LV_OBJ_FLAG_HIDDEN);
  if (lblWifiState) lv_obj_clear_flag(lblWifiState, LV_OBJ_FLAG_HIDDEN);
  if (tab == 0) {
    lv_obj_clear_flag(wifiPanel, LV_OBJ_FLAG_HIDDEN);
  } else if (tab == 1) {
    lv_obj_clear_flag(srvPanel, LV_OBJ_FLAG_HIDDEN);
    if (lblWifiState) lv_obj_add_flag(lblWifiState, LV_OBJ_FLAG_HIDDEN);
  } else if (tab == 2 && themePanel) {
    lv_obj_clear_flag(themePanel, LV_OBJ_FLAG_HIDDEN);
    if (lblWifiState) lv_obj_add_flag(lblWifiState, LV_OBJ_FLAG_HIDDEN);
  }
  lv_scr_load(scrSetup);
}
void updateWifiStatus() {
  char t[96];
  if (wst == W_ONLINE) {
    snprintf(t, sizeof(t), "WiFi: %s  %s", cfgSsid.c_str(), WiFi.localIP().toString().c_str());
    lv_obj_set_style_text_color(lblWifiState, C_OK, 0);
  } else if (wst == W_CONNECTING) {
    snprintf(t, sizeof(t), "Connecting to %s ...", pendingSsid.length() ? pendingSsid.c_str() : cfgSsid.c_str());
    lv_obj_set_style_text_color(lblWifiState, C_WARN, 0);
  } else {
    snprintf(t, sizeof(t), "WiFi: %s", cfgSsid.length() ? cfgSsid.c_str() : "not configured");
    lv_obj_set_style_text_color(lblWifiState, C_MUT, 0);
  }
  lv_label_set_text(lblWifiState, t);
  if (lblScanStatus) {
    if (setupMsg.length()) lv_label_set_text(lblScanStatus, setupMsg.c_str());
    else lv_label_set_text(lblScanStatus, L("Tap a network,\nthen enter\nthe password.", "点选网络，\n然后输入\n密码。"));
  }
}
static String scanSsid[20];
int scanCount = 0;
void doScan() {
  setupMsg = L("scanning...", "扫描中…");
  updateWifiStatus();
  int n = WiFi.scanNetworks();
  scanCount = 0;
  lv_obj_clean(apList);
  for (int i = 0; i < n && scanCount < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (int k = 0; k < scanCount; k++) if (scanSsid[k] == s) { dup = true; break; }
    if (dup) continue;
    scanSsid[scanCount] = s;
    bool locked = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    lv_obj_t *b = lv_list_add_btn(apList, locked ? LV_SYMBOL_CLOSE : LV_SYMBOL_WIFI, s.c_str());
    lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
    lv_obj_set_user_data(b, (void *)(intptr_t)scanCount);
    lv_obj_add_event_cb(b, [](lv_event_t *e) {
      lv_obj_t *btn = lv_event_get_target(e);
      int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
      if (idx < 0 || idx >= scanCount) return;
      pendingSsid = scanSsid[idx];
      editTarget = 9;
      lv_label_set_text(editTitle, (pendingSsid + L("  password", "  密码")).c_str());
      lv_textarea_set_password_mode(editTa, true);
      lv_textarea_set_text(editTa, "");
      lv_keyboard_set_textarea(editKb, editTa);
      lv_obj_clear_flag(editOverlay, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_CLICKED, NULL);
    scanCount++;
  }
  WiFi.scanDelete();
  setupMsg = scanCount ? "" : L("no networks found", "未发现网络");
  updateWifiStatus();
}
void openEditor(int target, const char *title, const char *initial, bool password) {
  editTarget = target;
  lv_label_set_text(editTitle, title);
  lv_textarea_set_password_mode(editTa, password);
  lv_textarea_set_text(editTa, initial ? initial : "");
  lv_keyboard_set_textarea(editKb, editTa);
  lv_obj_clear_flag(editOverlay, LV_OBJ_FLAG_HIDDEN);
}
void buildSetup() {
  scrSetup = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrSetup, C_BG, 0);
  lv_obj_set_style_border_width(scrSetup, 0, 0);
  lv_obj_clear_flag(scrSetup, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(scrSetup, 0, 0);

  lv_obj_t *bar = mkCard(scrSetup, 0, 0, UI_W, 34, C_HEAD);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_t *sttl = mkLabel(bar, L("SETUP", "设置"), C_ACCENT, 12, 7, 16);
  lv_obj_align(sttl, LV_ALIGN_LEFT_MID, 12, 0);
  lv_obj_t *bw = lv_btn_create(bar);
  lv_obj_set_size(bw, 62, 24);
  lv_obj_align(bw, LV_ALIGN_RIGHT_MID, -190, 0);
  tabBtn[0] = bw;
  lv_obj_set_style_bg_color(bw, C_ACCENT, 0);
  lv_obj_set_style_shadow_width(bw, 0, 0);
  lv_obj_t *lw = lv_label_create(bw);
  lv_label_set_text(lw, "WiFi");
  lv_obj_set_style_text_font(lw, &cn_font_12, 0);
  lv_obj_center(lw);
  lv_obj_add_event_cb(bw, [](lv_event_t *) { showSetupTab(0); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bs = lv_btn_create(bar);
  lv_obj_set_size(bs, 62, 24);
  lv_obj_align(bs, LV_ALIGN_RIGHT_MID, -120, 0);
  tabBtn[1] = bs;
  lv_obj_set_style_bg_color(bs, C_CARD, 0);
  lv_obj_set_style_shadow_width(bs, 0, 0);
  lv_obj_t *ls = lv_label_create(bs);
  lv_label_set_text(ls, L("Servers", "服务器"));
  lv_obj_set_style_text_font(ls, &cn_font_12, 0);
  lv_obj_center(ls);
  lv_obj_add_event_cb(bs, [](lv_event_t *) { showSetupTab(1); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bt = lv_btn_create(bar);
  lv_obj_set_size(bt, 62, 24);
  lv_obj_align(bt, LV_ALIGN_RIGHT_MID, -50, 0);
  tabBtn[2] = bt;
  lv_obj_set_style_bg_color(bt, C_CARD, 0);
  lv_obj_set_style_shadow_width(bt, 0, 0);
  lv_obj_t *lt = lv_label_create(bt);
  lv_label_set_text(lt, L("Theme", "主题"));
  lv_obj_set_style_text_font(lt, &cn_font_12, 0);
  lv_obj_center(lt);
  lv_obj_add_event_cb(bt, [](lv_event_t *) { showSetupTab(2); }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *bb = lv_btn_create(bar);
  lv_obj_set_size(bb, 36, 24);
  lv_obj_align(bb, LV_ALIGN_RIGHT_MID, -6, 0);
  lv_obj_set_style_bg_color(bb, C_CARD, 0);
  lv_obj_set_style_shadow_width(bb, 0, 0);
  lv_obj_t *lb = lv_label_create(bb);
  lv_label_set_text(lb, LV_SYMBOL_LEFT);
  lv_obj_center(lb);
  lv_obj_add_event_cb(bb, [](lv_event_t *) { if (scrDash) lv_scr_load(scrDash); }, LV_EVENT_CLICKED, NULL);

  lblWifiState = mkLabel(scrSetup, "WiFi: --", C_MUT, 12, 40, 12);

  wifiPanel = lv_obj_create(scrSetup);
  lv_obj_set_size(wifiPanel, UI_W, 280);
  lv_obj_set_pos(wifiPanel, 0, 40);
  lv_obj_set_style_bg_opa(wifiPanel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(wifiPanel, 0, 0);
  lv_obj_set_style_pad_all(wifiPanel, 0, 0);
  lv_obj_clear_flag(wifiPanel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *scanBtn = lv_btn_create(wifiPanel);
  lv_obj_set_size(scanBtn, 92, 30);
  lv_obj_set_pos(scanBtn, 376, 2);
  lv_obj_set_style_bg_color(scanBtn, C_CARD, 0);
  lv_obj_set_style_shadow_width(scanBtn, 0, 0);
  lv_obj_t *sl = lv_label_create(scanBtn);
  lv_label_set_text(sl, L("Scan", "扫描"));
  lv_obj_set_style_text_font(sl, &cn_font_14, 0);
  lv_obj_center(sl);
  lv_obj_add_event_cb(scanBtn, [](lv_event_t *) { doScan(); }, LV_EVENT_CLICKED, NULL);

  apList = lv_list_create(wifiPanel);
  lv_obj_set_size(apList, 356, 232);
  lv_obj_set_pos(apList, 8, 38);
  lv_obj_set_style_bg_color(apList, C_CARD, 0);
  lv_obj_set_style_border_width(apList, 0, 0);
  lv_obj_set_style_radius(apList, 8, 0);

  lblScanStatus = lv_label_create(wifiPanel);
  lv_obj_set_width(lblScanStatus, 96);
  lv_label_set_long_mode(lblScanStatus, LV_LABEL_LONG_WRAP);
  lv_label_set_text(lblScanStatus, L("Tap a network,\nthen enter\nthe password.", "点选网络，\n然后输入\n密码。"));
  lv_obj_set_style_text_color(lblScanStatus, C_MUT, 0);
  lv_obj_set_style_text_font(lblScanStatus, &cn_font_12, 0);
  lv_obj_set_pos(lblScanStatus, 374, 44);

  srvPanel = lv_obj_create(scrSetup);
  lv_obj_set_size(srvPanel, UI_W, 280);
  lv_obj_set_pos(srvPanel, 0, 40);
  lv_obj_set_style_bg_opa(srvPanel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(srvPanel, 0, 0);
  lv_obj_set_style_pad_all(srvPanel, 0, 0);
  lv_obj_clear_flag(srvPanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(srvPanel, LV_OBJ_FLAG_HIDDEN);

  mkLabel(srvPanel, L("ComfyUI servers (ip:port, leave empty to disable)", "ComfyUI 服务器（ip:端口，留空停用）"), C_MUT, 10, 2, 12);
  for (int i = 0; i < MAX_HOSTS; i++) {
    lv_obj_t *row = mkCard(srvPanel, 8, 20 + i * 40, 464, 38, C_CARD);
    char cap[24];
    snprintf(cap, sizeof(cap), L("Server %d", "服务器 %d"), i + 1);
    mkLabel(row, cap, C_MUT, 2, 1, 12);
    lblSrvVal[i] = mkLabel(row, hostStr[i].length() ? (hostStr[i] + ":" + String(hostPort[i])).c_str() : L("(unused)", "（未使用）"),
                           hostStr[i].length() ? C_TXT : C_MUT, 2, 17, 14);
    lv_obj_t *eb = lv_btn_create(row);
    lv_obj_set_size(eb, 60, 26);
    lv_obj_align(eb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(eb, C_INNER, 0);
    lv_obj_set_style_shadow_width(eb, 0, 0);
    lv_obj_t *el = lv_label_create(eb);
    lv_label_set_text(el, L("Edit", "编辑"));
    lv_obj_set_style_text_font(el, &cn_font_14, 0);
    lv_obj_center(el);
    lv_obj_set_user_data(eb, (void *)(intptr_t)i);
    lv_obj_add_event_cb(eb, [](lv_event_t *e) {
      lv_obj_t *btn = lv_event_get_target(e);
      int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
      char cur[64] = "";
      if (hostStr[idx].length()) snprintf(cur, sizeof(cur), "%s:%d", hostStr[idx].c_str(), hostPort[idx]);
      openEditor(idx, L("Server ip[:port]", "服务器 ip[:端口]"), cur, false);
    }, LV_EVENT_CLICKED, NULL);
  }

  // Ultra mode: hardware vitals from the on-host assistant (port A)
  btnUltraToggle = lv_btn_create(srvPanel);
  lv_obj_set_size(btnUltraToggle, 120, 26);
  lv_obj_set_pos(btnUltraToggle, 8, 182);
  lv_obj_set_style_bg_color(btnUltraToggle, ultraEnabled ? C_ACCENT : C_INNER, 0);
  lv_obj_set_style_shadow_width(btnUltraToggle, 0, 0);
  lblUltraToggle = lv_label_create(btnUltraToggle);
  lv_label_set_text(lblUltraToggle, ultraEnabled ? L("Ultra: ON", "Ultra：开") : L("Ultra: OFF", "Ultra：关"));
  lv_obj_set_style_text_font(lblUltraToggle, &cn_font_14, 0);
  lv_obj_center(lblUltraToggle);
  lv_obj_add_event_cb(btnUltraToggle, [](lv_event_t *) {
    ultraEnabled = !ultraEnabled;
    saveCfg();
    lv_label_set_text(lblUltraToggle, ultraEnabled ? L("Ultra: ON", "Ultra：开") : L("Ultra: OFF", "Ultra：关"));
    lv_obj_set_style_bg_color(btnUltraToggle, ultraEnabled ? C_ACCENT : C_INNER, 0);
    if (nHosts > 0 && wst == W_ONLINE) {
      buildDashboard();
      lv_scr_load(scrDash);
      netEpoch++;
    }
  }, LV_EVENT_CLICKED, NULL);

  btnUltraPort = lv_btn_create(srvPanel);
  lv_obj_set_size(btnUltraPort, 130, 26);
  lv_obj_set_pos(btnUltraPort, 136, 182);
  lv_obj_set_style_bg_color(btnUltraPort, C_INNER, 0);
  lv_obj_set_style_shadow_width(btnUltraPort, 0, 0);
  lblUltraPort = lv_label_create(btnUltraPort);
  char up[32];
  snprintf(up, sizeof(up), L("Port A: %d", "端口 A：%d"), ultraPort);
  lv_label_set_text(lblUltraPort, up);
  lv_obj_set_style_text_font(lblUltraPort, &cn_font_14, 0);
  lv_obj_center(lblUltraPort);
  lv_obj_add_event_cb(btnUltraPort, [](lv_event_t *) {
    char cur[12];
    snprintf(cur, sizeof(cur), "%d", ultraPort);
    openEditor(10, L("Assistant port A", "看板助手端口 A"), cur, false);
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *ultraHint = mkLabel(srvPanel, L("Ultra overview adds CPU/GPU/RAM/VRAM load + power from the assistant.",
                      "Ultra 总览由看板助手补充 CPU/GPU/内存/显存占用与功耗。"), C_MUT, 274, 178, 12);
  lv_obj_set_width(ultraHint, 198);
  lv_label_set_long_mode(ultraHint, LV_LABEL_LONG_WRAP);

  mkLabel(srvPanel, L("Language", "语言"), C_MUT, 10, 212, 12);
  lv_obj_t *lbEn = lv_btn_create(srvPanel);
  lv_obj_set_size(lbEn, 52, 26);
  lv_obj_set_pos(lbEn, 10, 230);
  lv_obj_set_style_bg_color(lbEn, lang == 0 ? C_ACCENT : C_INNER, 0);
  lv_obj_set_style_shadow_width(lbEn, 0, 0);
  lv_obj_t *le = lv_label_create(lbEn);
  lv_label_set_text(le, "EN");
  lv_obj_set_style_text_font(le, &cn_font_14, 0);
  lv_obj_center(le);
  lv_obj_add_event_cb(lbEn, [](lv_event_t *) {
    prefs.begin("cfg", false); prefs.putInt("lang", 0); prefs.end();
    delay(120); ESP.restart();
  }, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lbZh = lv_btn_create(srvPanel);
  lv_obj_set_size(lbZh, 52, 26);
  lv_obj_set_pos(lbZh, 68, 230);
  lv_obj_set_style_bg_color(lbZh, lang == 1 ? C_ACCENT : C_INNER, 0);
  lv_obj_set_style_shadow_width(lbZh, 0, 0);
  lv_obj_t *lz = lv_label_create(lbZh);
  lv_label_set_text(lz, "中文");
  lv_obj_set_style_text_font(lz, &cn_font_14, 0);
  lv_obj_center(lz);
  lv_obj_add_event_cb(lbZh, [](lv_event_t *) {
    prefs.begin("cfg", false); prefs.putInt("lang", 1); prefs.end();
    delay(120); ESP.restart();
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *okBtn = lv_btn_create(srvPanel);
  lv_obj_set_size(okBtn, 150, 36);
  lv_obj_align(okBtn, LV_ALIGN_BOTTOM_RIGHT, -10, -6);
  lv_obj_set_style_bg_color(okBtn, C_ACCENT, 0);
  lv_obj_set_style_shadow_width(okBtn, 0, 0);
  lv_obj_t *okl = lv_label_create(okBtn);
  lv_label_set_text(okl, L("Save & start", "保存并启动"));
  lv_obj_set_style_text_font(okl, &cn_font_14, 0);
  lv_obj_center(okl);
  lv_obj_add_event_cb(okBtn, [](lv_event_t *) {
    int cnt = 0;
    for (int i = 0; i < MAX_HOSTS; i++) {
      if (!hostStr[i].length()) continue;
      if (cnt != i) { hostStr[cnt] = hostStr[i]; hostPort[cnt] = hostPort[i]; }
      cnt++;
    }
    for (int i = cnt; i < MAX_HOSTS; i++) { hostStr[i] = ""; hostPort[i] = DEFAULT_PORT; }
    nHosts = cnt;
    saveCfg();
    netConfigure();
    if (nHosts > 0 && wst == W_ONLINE) {
      activeHost = 0; activePage = 0; activePageIdx = 0;
      buildDashboard();
      lv_scr_load(scrDash);
      netEpoch++;
    } else showSetupTab(0);
  }, LV_EVENT_CLICKED, NULL);

  themePanel = lv_obj_create(scrSetup);
  lv_obj_set_size(themePanel, UI_W, 280);
  lv_obj_set_pos(themePanel, 0, 40);
  lv_obj_set_style_bg_opa(themePanel, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(themePanel, 0, 0);
  lv_obj_set_style_pad_all(themePanel, 0, 0);
  lv_obj_clear_flag(themePanel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(themePanel, LV_OBJ_FLAG_HIDDEN);

  mkLabel(themePanel, L("Layout (auto, follows server count)", "布局（按服务器数量自动）"), C_MUT, 10, 2, 12);
  char lay[72];
  if (useGrid()) {
    if (lang) snprintf(lay, sizeof(lay), "%d 台 → %d 列 × %d 行（总览 / 日志 / 产物）", nHosts, gridCols(), gridRows());
    else snprintf(lay, sizeof(lay), "%d hosts -> %dx%d grid (OVERVIEW / LOGS / OUTPUTS)", nHosts, gridCols(), gridRows());
  } else {
    if (lang) snprintf(lay, sizeof(lay), "%d 台 → 详情页 + 产物页", nHosts);
    else snprintf(lay, sizeof(lay), "%d host -> detail + outputs", nHosts);
  }
  lv_obj_t *layLbl = mkLabel(themePanel, lay, C_TXT, 10, 18, 14);
  lv_obj_set_width(layLbl, 460);
  lv_label_set_long_mode(layLbl, LV_LABEL_LONG_DOT);
  mkLabel(themePanel, L("Add or clear servers on the Servers page.", "服务器数量在「服务器」页设置。"), C_MUT, 10, 38, 12);
  mkLabel(themePanel, L("Theme (tap to apply, reboots)", "主题（点选应用并重启）"), C_MUT, 10, 56, 12);
  for (int i = 0; i < 10; i++) {
    int x = 8 + (i % 2) * 232;
    int y = 74 + (i / 2) * 38;
    lv_obj_t *tb = lv_btn_create(themePanel);
    lv_obj_set_size(tb, 224, 34);
    lv_obj_set_pos(tb, x, y);
    lv_obj_set_style_bg_color(tb, lv_color_hex(THEMES[i].card), 0);
    lv_obj_set_style_border_width(tb, i == themeIdx ? 2 : 1, 0);
    lv_obj_set_style_border_color(tb, lv_color_hex(i == themeIdx ? THEMES[i].accent : THEMES[i].line), 0);
    lv_obj_set_style_shadow_width(tb, 0, 0);
    lv_obj_t *sw = lv_obj_create(tb);
    lv_obj_set_size(sw, 12, 12);
    lv_obj_align(sw, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(sw, 3, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(THEMES[i].accent), 0);
    lv_obj_set_style_border_width(sw, 0, 0);
    lv_obj_t *nl = lv_label_create(tb);
    lv_label_set_text(nl, L(THEMES[i].name, THEME_ZH[i]));
    lv_obj_set_style_text_color(nl, lv_color_hex(THEMES[i].txt), 0);
    lv_obj_set_style_text_font(nl, &cn_font_12, 0);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 26, 0);
    lv_obj_set_user_data(tb, (void *)(intptr_t)i);
    lv_obj_add_event_cb(tb, [](lv_event_t *e) {
      lv_obj_t *btn = lv_event_get_target(e);
      int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
      prefs.begin("cfg", false); prefs.putInt("theme", idx); prefs.end();
      delay(120); ESP.restart();
    }, LV_EVENT_CLICKED, NULL);
  }

  editOverlay = lv_obj_create(scrSetup);
  lv_obj_set_size(editOverlay, UI_W, UI_H);
  lv_obj_set_pos(editOverlay, 0, 0);
  lv_obj_set_style_bg_color(editOverlay, C_BG, 0);
  lv_obj_set_style_bg_opa(editOverlay, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(editOverlay, 0, 0);
  lv_obj_set_style_pad_all(editOverlay, 0, 0);
  lv_obj_clear_flag(editOverlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(editOverlay, LV_OBJ_FLAG_HIDDEN);

  editTitle = mkLabel(editOverlay, "Edit", C_ACCENT, 12, 8, 16);
  editTa = lv_textarea_create(editOverlay);
  lv_obj_set_size(editTa, 440, 44);
  lv_obj_set_pos(editTa, 16, 36);
  lv_textarea_set_one_line(editTa, true);
  lv_obj_set_style_text_font(editTa, &cn_font_16, 0);

  editKb = lv_keyboard_create(editOverlay);
  lv_obj_set_size(editKb, UI_W, 210);
  lv_obj_align(editKb, LV_ALIGN_BOTTOM_MID, 0, 0);

  lv_obj_t *ok = lv_btn_create(editOverlay);
  lv_obj_set_size(ok, 80, 30);
  lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -100, 4);
  lv_obj_set_style_bg_color(ok, C_ACCENT, 0);
  lv_obj_set_style_shadow_width(ok, 0, 0);
  lv_obj_t *okl2 = lv_label_create(ok);
  lv_label_set_text(okl2, L("OK", "确定"));
  lv_obj_set_style_text_font(okl2, &cn_font_14, 0);
  lv_obj_center(okl2);
  lv_obj_add_event_cb(ok, [](lv_event_t *) {
    const char *txt = lv_textarea_get_text(editTa);
    if (editTarget == 9) {
      cfgSsid = pendingSsid.length() ? pendingSsid : cfgSsid;
      cfgPsk = txt;
      saveCfg();
      WiFi.disconnect();
      WiFi.begin(cfgSsid.c_str(), cfgPsk.c_str());
      wifiTrySince = millis();
      wst = W_CONNECTING;
      updateWifiStatus();
    } else if (editTarget == 10) {
      int p = String(txt).toInt();
      if (p > 0 && p <= 65535) {
        ultraPort = p;
        saveCfg();
        char t[32];
        snprintf(t, sizeof(t), L("Port A: %d", "端口 A：%d"), ultraPort);
        lv_label_set_text(lblUltraPort, t);
        lockNet();
        for (int i = 0; i < MAX_HOSTS; i++) net[i].tU = 0;
        unlockNet();
        netEpoch++;
      }
    } else if (editTarget >= 0 && editTarget < MAX_HOSTS) {
      parseHost(editTarget, String(txt));
      netConfigure();
      if (hostStr[editTarget].length()) {
        char t[64];
        snprintf(t, sizeof(t), "%s:%d", hostStr[editTarget].c_str(), hostPort[editTarget]);
        lv_label_set_text(lblSrvVal[editTarget], t);
        lv_obj_set_style_text_color(lblSrvVal[editTarget], C_TXT, 0);
      } else {
        lv_label_set_text(lblSrvVal[editTarget], "(unused)");
        lv_obj_set_style_text_color(lblSrvVal[editTarget], C_MUT, 0);
      }
    }
    lv_obj_add_flag(editOverlay, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *cancel = lv_btn_create(editOverlay);
  lv_obj_set_size(cancel, 80, 30);
  lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, -12, 4);
  lv_obj_set_style_bg_color(cancel, C_CARD, 0);
  lv_obj_set_style_shadow_width(cancel, 0, 0);
  lv_obj_t *cl = lv_label_create(cancel);
  lv_label_set_text(cl, LV_SYMBOL_CLOSE);
  lv_obj_center(cl);
  lv_obj_add_event_cb(cancel, [](lv_event_t *) { lv_obj_add_flag(editOverlay, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, NULL);

  lv_scr_load(scrSetup);
}

// ================= dashboard =================
void updatePageDots() {
  int used = pageCount();
  const char *sub;
  if (useGrid()) sub = activePageIdx == 0 ? L("OVERVIEW", "总览") : (activePageIdx == 1 ? L("LOGS", "日志") : L("OUT", "产物"));
  else sub = activePage ? L("OUT", "产物") : L("MON", "监控");
  setLbl(lblPageSub, sub);

  // approximate text width for the page label (12px: CJK 12, latin 7)
  int labW = 0;
  for (const char *p = sub; *p; p++) {
    if ((*p & 0xC0) == 0x80) continue;  // utf8 continuation
    labW += ((unsigned char)*p >= 0xE0) ? 12 : 7;
  }
  int dotsW = 0;
  for (int i = 0; i < used; i++) dotsW += (i == activePageIdx ? 14 : 6) + (i < used - 1 ? 7 : 0);
  int groupW = dotsW + 6 + labW;
  int regionL = 212, regionR = 338;  // host ends ~208, wifi bars start 345
  int x = regionL + (regionR - regionL - groupW) / 2;
  if (x < regionL) x = regionL;
  for (int i = 0; i < 6; i++) {
    if (!pageDots[i]) continue;
    if (i >= used) { lv_obj_add_flag(pageDots[i], LV_OBJ_FLAG_HIDDEN); continue; }
    lv_obj_clear_flag(pageDots[i], LV_OBJ_FLAG_HIDDEN);
    bool act = (i == activePageIdx);
    int w = act ? 14 : 6;
    lv_obj_set_size(pageDots[i], w, 6);
    lv_obj_set_style_bg_color(pageDots[i], act ? C_ACCENT : C_MUT, 0);
    lv_obj_set_style_bg_opa(pageDots[i], act ? LV_OPA_COVER : LV_OPA_60, 0);
    lv_obj_set_pos(pageDots[i], x, 7);
    x += w + 7;
  }
  lv_obj_align(lblPageSub, LV_ALIGN_LEFT_MID, x + 6 - 7, -1);
}
void refreshHeader() {
  if (!scrDash || !lblHost) return;
  if (useGrid()) {
    int on = 0;
    for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) if (dview[h].serverOk) on++;
    char t[32];
    if (lang) snprintf(t, sizeof(t), "%d/%d 台在线", on, nHosts);
    else snprintf(t, sizeof(t), "%d/%d online", on, nHosts);
    setLbl(lblHost, t);
    lv_color_t dot = (on == 0) ? C_ERR : (on < nHosts ? C_WARN : C_OK);
    lv_obj_set_style_bg_color(dotServer, (wst == W_ONLINE) ? dot : C_ERR, 0);
    updatePageDots();
    return;
  }
  if (wst != W_ONLINE) {
    setLbl(lblHost, "wifi offline");
    lv_obj_set_style_bg_color(dotServer, C_ERR, 0);
  } else {
    char t[48];
    if (hostPort[activeHost] == DEFAULT_PORT) snprintf(t, sizeof(t), "%s", hostStr[activeHost].c_str());
    else snprintf(t, sizeof(t), "%s:%d", hostStr[activeHost].c_str(), hostPort[activeHost]);
    setLbl(lblHost, t);
    lv_obj_set_style_bg_color(dotServer, serverOk ? C_OK : C_ERR, 0);
  }
  updatePageDots();
}
void refreshWifiBars() {
  int rssi = (wst == W_ONLINE) ? WiFi.RSSI() : -127;
  int lvl = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
  lv_color_t col = lvl >= 3 ? C_OK : (lvl == 2 ? C_WARN : C_ERR);
  for (int i = 0; i < 4; i++) {
    if (!wifiBars[i]) continue;
    bool on = i < lvl;
    lv_obj_set_style_bg_color(wifiBars[i], on ? col : C_LINE, 0);
    lv_obj_set_style_bg_opa(wifiBars[i], on ? LV_OPA_COVER : LV_OPA_40, 0);
  }
}

void refreshClock() {
  refreshWifiBars();
  if (!lblClock) return;
  char t[16];
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(t, sizeof(t), "%H:%M:%S", &tmv);
  } else strcpy(t, "--:--:--");
  setLbl(lblClock, t);
}

static void buildMonitor(int idx, lv_obj_t *tile) {
  MonUI &m = mui[idx];
  lv_obj_t *band = mkCard(tile, 4, 4, 472, 74);
  m.lblState = mkLabel(band, "OFFLINE", C_ERR, 10, 0, 20);
  m.lblDur = mkLabel(band, "00:00", C_TXT, 0, 0, 20);
  lv_obj_align(m.lblDur, LV_ALIGN_TOP_RIGHT, -10, 0);
  m.lblNode = mkLabel(band, "no job", C_MUT, 10, 30, 12);
  m.lblQueue = mkLabel(band, "queue -", C_MUT, 0, 30, 12);
  lv_obj_align(m.lblQueue, LV_ALIGN_TOP_RIGHT, -10, 30);
  m.barProg = lv_bar_create(band);
  lv_obj_set_size(m.barProg, 286, 10);
  lv_obj_set_pos(m.barProg, 10, 48);
  lv_obj_set_style_bg_opa(m.barProg, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(m.barProg, lv_color_hex(0x2f4a80), 0);
  lv_obj_set_style_bg_color(m.barProg, C_OK, LV_PART_INDICATOR);
  lv_obj_set_style_border_width(m.barProg, 0, 0);
  lv_bar_set_range(m.barProg, 0, 100);
  lv_bar_set_value(m.barProg, 0, LV_ANIM_OFF);
  m.lblStep = mkLabel(band, "", C_TXT, 306, 45, 12);
  m.spinner = lv_spinner_create(band, 900, 40);
  lv_obj_set_size(m.spinner, 14, 14);
  lv_obj_set_pos(m.spinner, 12, 46);
  lv_obj_add_flag(m.spinner, LV_OBJ_FLAG_HIDDEN);
  m.lblHint = mkLabel(band, "waiting for sampler progress", C_MUT, 32, 45, 12);
  lv_obj_set_width(m.lblHint, 430);
  lv_label_set_long_mode(m.lblHint, LV_LABEL_LONG_DOT);

  lv_obj_t *gCard = mkCard(tile, 4, 84, 176, 198);
  if (ultraEnabled) {
    static const char *MLBL[4] = {"CPU", "GPU", "RAM", "VRAM"};
    for (int k = 0; k < 4; k++) {
      int y = 2 + k * 30;
      m.uLbl[k] = mkLabel(gCard, MLBL[k], C_MUT, 4, y, 12);
      m.uVal[k] = mkLabel(gCard, "--", C_TXT, 0, y, 12);
      lv_obj_align(m.uVal[k], LV_ALIGN_TOP_RIGHT, -4, y);
      m.uBar[k] = mkBarIn(gCard, 4, y + 16, 164, 8, C_ACCENT);
    }
    m.uPower = mkLabel(gCard, "", C_WARN, 4, 124, 12);
    lv_obj_set_width(m.uPower, 164);
    lv_label_set_long_mode(m.uPower, LV_LABEL_LONG_DOT);
    mkLabel(gCard, "GPU", C_MUT, 4, 146, 12);
    m.imgGpu = lv_img_create(gCard);
    lv_obj_set_pos(m.imgGpu, 34, 145);
    m.lblGpu = mkLabel(gCard, "--", C_TXT, 54, 146, 12);
    lv_obj_set_width(m.lblGpu, 114);
    lv_label_set_long_mode(m.lblGpu, LV_LABEL_LONG_DOT);
    m.lblVer = mkLabel(gCard, "ComfyUI --", C_MUT, 4, 166, 12);
    lv_obj_set_width(m.lblVer, 164);
    lv_label_set_long_mode(m.lblVer, LV_LABEL_LONG_DOT);
  } else {
    for (int i = 0; i < 2; i++) {
      int y = 2 + i * 46;
      mkLabel(gCard, i == 0 ? "VRAM" : "RAM", C_MUT, 4, y, 12);
      m.gVal[i] = mkLabel(gCard, "--", C_TXT, 0, y, 12);
      lv_obj_align(m.gVal[i], LV_ALIGN_TOP_RIGHT, -4, y);
      m.gBar[i] = lv_bar_create(gCard);
      lv_obj_set_size(m.gBar[i], 164, 12);
      lv_obj_set_pos(m.gBar[i], 4, y + 20);
      lv_bar_set_range(m.gBar[i], 0, 100);
      lv_obj_set_style_bg_opa(m.gBar[i], LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(m.gBar[i], lv_color_hex(0x1a2947), 0);
      lv_obj_set_style_bg_color(m.gBar[i], C_ACCENT, LV_PART_INDICATOR);
      lv_bar_set_value(m.gBar[i], 0, LV_ANIM_OFF);
    }
    mkLabel(gCard, "GPU", C_MUT, 4, 98, 12);
    m.imgGpu = lv_img_create(gCard);
    lv_obj_set_pos(m.imgGpu, 4, 111);
    m.lblGpu = mkLabel(gCard, "--", C_TXT, 24, 112, 12);
    lv_obj_set_width(m.lblGpu, 144);
    lv_label_set_long_mode(m.lblGpu, LV_LABEL_LONG_DOT);
    m.lblAcc = mkLabel(gCard, "", C_MUT, 4, 128, 12);
    lv_obj_set_width(m.lblAcc, 164);
    lv_label_set_long_mode(m.lblAcc, LV_LABEL_LONG_DOT);
    m.lblVer = mkLabel(gCard, "ComfyUI --", C_MUT, 4, 148, 12);
    mkLabel(gCard, L("swipe for outputs >", "右滑查看产物 >"), C_MUT, 4, 168, 12);
  }

  lv_obj_t *lCard = mkCard(tile, 186, 84, 290, 198);
  mkLabel(lCard, L("RUN LOG", "运行日志"), C_MUT, 8, 4, 12);
  m.logLabel = lv_label_create(lCard);
  lv_label_set_text(m.logLabel, "");
  lv_obj_set_style_text_color(m.logLabel, C_TXT, 0);
  lv_obj_set_style_text_font(m.logLabel, &cn_font_12, 0);
  lv_obj_set_width(m.logLabel, 270);
  lv_obj_set_height(m.logLabel, 164);
  lv_label_set_long_mode(m.logLabel, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(m.logLabel, 8, 24);
}

static void buildOutputs(int idx, lv_obj_t *tile) {
  OutUI &o = oui[idx];
  mkLabel(tile, L("RECENT OUTPUTS", "最近产物"), C_ACCENT, 10, 6, 16);
  lv_obj_t *hint = mkLabel(tile, L("thumbnails: image preview or video poster via /view", "缩略图：图片预览或视频封面 (/view)"), C_MUT, 10, 30, 12);
  (void)hint;
  for (int k = 0; k < MAX_JOBS; k++) {
    int x = 4 + (k % 2) * 240;
    int y = 50 + (k / 2) * 118;
    lv_obj_t *card = mkCard(tile, x, y, 232, 112, C_CARD);
    lv_obj_t *ph = lv_obj_create(card);
    lv_obj_set_size(ph, THUMB_W, THUMB_H);
    lv_obj_set_pos(ph, 0, 0);
    lv_obj_set_style_bg_color(ph, C_INNER, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_set_style_radius(ph, 4, 0);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
    o.ph[k] = lv_label_create(ph);
    lv_label_set_text(o.ph[k], "no preview");
    lv_obj_set_style_text_color(o.ph[k], C_MUT, 0);
    lv_obj_set_style_text_font(o.ph[k], &cn_font_12, 0);
    lv_obj_center(o.ph[k]);

    o.canvas[k] = lv_canvas_create(card);
    lv_color_t *buf = (lv_color_t *)heap_caps_malloc(THUMB_W * THUMB_H * 2, MALLOC_CAP_SPIRAM);
    if (buf) {
      lv_canvas_set_buffer(o.canvas[k], buf, THUMB_W, THUMB_H, LV_IMG_CF_TRUE_COLOR);
      lv_canvas_fill_bg(o.canvas[k], lv_color_hex(0x101828), LV_OPA_COVER);
      lv_obj_set_pos(o.canvas[k], 0, 0);
      lv_obj_add_flag(o.canvas[k], LV_OBJ_FLAG_HIDDEN);
    }
    o.name[k] = mkLabel(card, "--", C_TXT, 106, 0, 12);
    lv_obj_set_width(o.name[k], 120);
    lv_obj_set_height(o.name[k], 16);
    lv_label_set_long_mode(o.name[k], LV_LABEL_LONG_DOT);
    o.meta[k] = mkLabel(card, "", C_MUT, 106, 18, 12);
    lv_obj_set_width(o.meta[k], 120);
    lv_obj_set_height(o.meta[k], 66);
    lv_label_set_long_mode(o.meta[k], LV_LABEL_LONG_CLIP);
  }
}

bool pullHost(int h) {
  if (h < 0 || h >= nHosts || h >= MAX_HOSTS) return false;
  bool changed = false;
  lockNet();
  NetData &nd = net[h];
  if (nd.ver != dviewVer[h]) {
    dview[h].vitals = nd.vitals;
    dview[h].run = nd.run;
    dview[h].ultra = nd.ultra;
    dview[h].nJobs = nd.nJobs;
    for (int i = 0; i < nd.nJobs && i < MAX_JOBS; i++) dview[h].jobs[i] = nd.jobs[i];
    dview[h].serverOk = nd.serverOk;
    dview[h].everSeen = nd.everSeen;
    dview[h].nodeTotal = nd.nodeTotal;
    dview[h].nodesDone = nd.nodesDone;
    dview[h].nodesCached = nd.nodesCached;
    dviewVer[h] = nd.ver;
    changed = true;
  }
  unlockNet();
  return changed;
}

static lv_obj_t *mkBarIn(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t ind) {
  lv_obj_t *b = lv_bar_create(parent);
  lv_obj_set_size(b, w, h);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(TH.line), 0);
  lv_obj_set_style_bg_color(b, ind, LV_PART_INDICATOR);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_bar_set_range(b, 0, 100);
  lv_bar_set_value(b, 0, LV_ANIM_OFF);
  return b;
}

static void hostStateText(const HostView &v, char *state, size_t n, lv_color_t &col) {
  if (wst != W_ONLINE) { strlcpy(state, L("WIFI?", "无网络"), n); col = C_ERR; }
  else if (!v.serverOk) {
    bool linking = stillLinking(v.everSeen);
    strlcpy(state, linking ? L("LINKING", "连接中") : L("OFFLINE", "离线"), n);
    col = linking ? C_WARN : C_ERR;
  }
  else if (v.run.running) { strlcpy(state, L("GENERATING", "生成中"), n); col = C_OK; }
  else if (v.run.pending > 0) { strlcpy(state, L("QUEUED", "排队中"), n); col = C_WARN; }
  else { strlcpy(state, L("IDLE", "空闲"), n); col = C_ACCENT; }
}

// compact one-word form for the log header rows, where the ip needs the room
static const char *hostStateShort(const HostView &v, lv_color_t &col) {
  if (wst != W_ONLINE) { col = C_ERR; return L("WIFI", "网络"); }
  if (!v.serverOk) {
    bool linking = stillLinking(v.everSeen);
    col = linking ? C_WARN : C_ERR;
    return linking ? L("LINK", "连接") : L("OFF", "离线");
  }
  if (v.run.running) { col = C_OK; return L("RUN", "生成"); }
  if (v.run.pending > 0) { col = C_WARN; return L("QUEUE", "排队"); }
  col = C_ACCENT;
  return L("IDLE", "空闲");
}

static const char *cellTitle(int h) {
  static char buf[MAX_HOSTS][56];
  if (!hostStr[h].length()) { strlcpy(buf[h], L("(unused)", "（未使用）"), 56); return buf[h]; }
  if (hostPort[h] == DEFAULT_PORT) snprintf(buf[h], 56, "%s", hostStr[h].c_str());
  else snprintf(buf[h], 56, "%s:%d", hostStr[h].c_str(), hostPort[h]);
  return buf[h];
}

// One overview cell. Rows are added while they still fit, so 1x1 / 2x1 / 3x1 / 2x2
// all come out of this builder; the queue moves into the title row when there is
// no free row left.
static void buildDashCell(int h, lv_obj_t *tile, int x, int y, int cw, int ch) {
  DashCell &c = gdash[h];
  lv_obj_t *card = mkCard(tile, x, y, cw, ch);
  int iw = cw - 12, ih = ch - 12;
  bool tight = (ih < 170);            // 2x2 cells: text-only memory rows, smaller state font
  int gap = (ih >= 240) ? 6 : 0;
  int cy = 0;

  c.title = mkLabel(card, cellTitle(h), C_MUT, 0, cy, 12);
  lv_obj_set_width(c.title, iw);
  lv_label_set_long_mode(c.title, LV_LABEL_LONG_DOT);
  cy += 16;

  int sf = (!tight && iw >= 200) ? 20 : 16;
  c.state = mkLabel(card, "--", C_ACCENT, 0, cy, sf);
  c.dur = mkLabel(card, "00:00", C_TXT, 0, cy, sf);
  lv_obj_align(c.dur, LV_ALIGN_TOP_RIGHT, 0, cy);
  cy += sf + 4 + gap;

  c.node = mkLabel(card, "", C_MUT, 0, cy, 12);
  lv_obj_set_width(c.node, iw - 56);
  lv_label_set_long_mode(c.node, LV_LABEL_LONG_DOT);
  c.step = mkLabel(card, "", C_TXT, 0, cy, 12);
  lv_obj_align(c.step, LV_ALIGN_TOP_RIGHT, 0, cy);
  cy += 16 + gap;

  c.bar = mkBarIn(card, 0, cy, iw, 10, C_OK);
  cy += 14 + gap;

  if (cy + 14 <= ih) {
    mkLabel(card, "VRAM", C_MUT, 0, cy, 12);
    c.vramVal = mkLabel(card, "--", C_TXT, 0, cy, 12);
    lv_obj_align(c.vramVal, LV_ALIGN_TOP_RIGHT, 0, cy);
    cy += 14;
    if (!tight && cy + 12 <= ih) {
      c.vbar = mkBarIn(card, 0, cy, iw, 10, C_ACCENT);
      cy += 14 + gap;
    }
  }
  if (cy + 14 <= ih) {
    mkLabel(card, "RAM", C_MUT, 0, cy, 12);
    c.ramVal = mkLabel(card, "--", C_TXT, 0, cy, 12);
    lv_obj_align(c.ramVal, LV_ALIGN_TOP_RIGHT, 0, cy);
    cy += 14;
    if (!tight && cy + 12 <= ih) {
      c.rbar = mkBarIn(card, 0, cy, iw, 10, C_ACCENT);
      cy += 14 + gap;
    }
  }
  if (cy + 14 <= ih) {
    c.img = lv_img_create(card);
    lv_obj_set_pos(c.img, 0, cy - 1);
    c.gpu = mkLabel(card, "--", C_TXT, 20, cy, 12);
    lv_obj_set_width(c.gpu, iw - 20);
    lv_label_set_long_mode(c.gpu, LV_LABEL_LONG_DOT);
    cy += 14;
    if (cy + 14 <= ih) {
      c.acc = mkLabel(card, "", C_MUT, 0, cy, 12);
      lv_obj_set_width(c.acc, iw);
      lv_label_set_long_mode(c.acc, LV_LABEL_LONG_DOT);
      cy += 14 + gap;
    }
  }
  if (cy + 14 <= ih) {
    c.ver = mkLabel(card, "", C_MUT, 0, cy, 12);
    lv_obj_set_width(c.ver, iw);
    lv_label_set_long_mode(c.ver, LV_LABEL_LONG_DOT);
    cy += 14;
  }
  if (cy + 14 <= ih) {
    c.queue = mkLabel(card, "", C_MUT, 0, cy, 12);
  } else {
    c.queue = mkLabel(card, "", C_MUT, 0, 0, 12);
    lv_obj_align(c.queue, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_width(c.title, iw - 44);
  }
}

static void buildLogsCell(int h, lv_obj_t *tile, int x, int y, int cw, int ch) {
  LogCell &c = glog[h];
  lv_obj_t *card = mkCard(tile, x, y, cw, ch);
  int iw = cw - 12, ih = ch - 12;
  c.title = mkLabel(card, cellTitle(h), C_MUT, 0, 0, 12);
  lv_obj_set_width(c.title, iw - 40);
  lv_label_set_long_mode(c.title, LV_LABEL_LONG_DOT);
  c.state = mkLabel(card, "--", C_MUT, 0, 0, 12);
  lv_obj_align(c.state, LV_ALIGN_TOP_RIGHT, 0, 0);
  c.log = lv_label_create(card);
  lv_label_set_text(c.log, "");
  lv_obj_set_style_text_color(c.log, C_TXT, 0);
  lv_obj_set_style_text_font(c.log, &cn_font_12, 0);
  lv_obj_set_width(c.log, iw);
  lv_obj_set_height(c.log, ih - 18);
  lv_label_set_long_mode(c.log, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(c.log, 0, 18);
  c.lines = (ih - 18) / 15;
  if (c.lines < 1) c.lines = 1;
}

static void buildOutCell(int h, lv_obj_t *tile, int x, int y, int cw, int ch) {
  OutCell &c = gout[h];
  lv_obj_t *card = mkCard(tile, x, y, cw, ch);
  int iw = cw - 12, ih = ch - 12;
  c.title = mkLabel(card, cellTitle(h), C_MUT, 0, 0, 12);
  lv_obj_set_width(c.title, iw);
  lv_label_set_long_mode(c.title, LV_LABEL_LONG_DOT);
  c.jobs = (iw >= 200 && ih >= 200) ? 2 : 1;
  bool sideBySide = (iw >= 200);
  for (int k = 0; k < c.jobs; k++) {
    lv_obj_t *item;
    int tx, ty, tw, th;
    if (sideBySide) {
      int itemH = (ih - 20 - (c.jobs - 1) * 4) / c.jobs;
      item = mkCard(card, 0, 20 + k * (itemH + 4), iw, itemH, C_INNER);
      tx = 106; ty = 0; tw = iw - 112; th = itemH - 6;
    } else {
      item = mkCard(card, 0, 20 + k * (ih - 20), iw, ih - 20, C_INNER);
      tx = 0; ty = THUMB_H + 4; tw = iw; th = ih - 20 - 12 - ty;
    }
    int tx2 = sideBySide ? 0 : (iw - THUMB_W) / 2;
    lv_obj_t *ph = lv_obj_create(item);
    lv_obj_set_size(ph, THUMB_W, THUMB_H);
    lv_obj_set_pos(ph, tx2, 0);
    lv_obj_set_style_bg_color(ph, C_CARD, 0);
    lv_obj_set_style_border_width(ph, 0, 0);
    lv_obj_set_style_radius(ph, 4, 0);
    lv_obj_set_style_pad_all(ph, 0, 0);
    lv_obj_clear_flag(ph, LV_OBJ_FLAG_SCROLLABLE);
    c.ph[k] = lv_label_create(ph);
    lv_label_set_text(c.ph[k], L("loading...", "加载中…"));
    lv_obj_set_style_text_color(c.ph[k], C_MUT, 0);
    lv_obj_set_style_text_font(c.ph[k], &cn_font_12, 0);
    lv_obj_set_width(c.ph[k], THUMB_W - 4);
    lv_label_set_long_mode(c.ph[k], LV_LABEL_LONG_WRAP);
    lv_obj_center(c.ph[k]);
    lv_color_t *buf = (lv_color_t *)heap_caps_malloc(THUMB_W * THUMB_H * 2, MALLOC_CAP_SPIRAM);
    c.cv[k] = lv_canvas_create(item);
    if (buf) {
      lv_canvas_set_buffer(c.cv[k], buf, THUMB_W, THUMB_H, LV_IMG_CF_TRUE_COLOR);
      lv_canvas_fill_bg(c.cv[k], lv_color_hex(TH.inner), LV_OPA_COVER);
    }
    lv_obj_set_pos(c.cv[k], tx2, 0);
    lv_obj_add_flag(c.cv[k], LV_OBJ_FLAG_HIDDEN);
    c.name[k] = mkLabel(item, "--", C_TXT, tx, ty, 12);
    lv_obj_set_width(c.name[k], tw);
    lv_obj_set_height(c.name[k], 16);
    lv_label_set_long_mode(c.name[k], LV_LABEL_LONG_DOT);
    c.meta[k] = mkLabel(item, "", C_MUT, tx, ty + 18, 12);
    lv_obj_set_width(c.meta[k], tw);
    lv_obj_set_height(c.meta[k], th > 20 ? th - 18 : 12);
    lv_label_set_long_mode(c.meta[k], LV_LABEL_LONG_CLIP);
  }
}

void refreshGridDash() {
  char t[72], st[16];
  for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) {
    DashCell &c = gdash[h];
    if (!c.state) continue;
    const HostView &v = dview[h];
    lv_color_t col;
    hostStateText(v, st, sizeof(st), col);
    setLbl(c.state, st);
    lv_obj_set_style_text_color(c.state, col, 0);
    uint32_t secs = (v.serverOk && v.run.running && v.run.start_ms) ? (millis() - v.run.start_ms) / 1000 : 0;
    if (v.serverOk) snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
    else strcpy(t, "--:--");
    setLbl(c.dur, t);
    if (!v.serverOk) snprintf(t, sizeof(t), "%s", stillLinking(v.everSeen) ? L("waiting...", "等待连接…") : L("no response", "无响应"));
    else if (v.run.running && v.run.node[0]) snprintf(t, sizeof(t), L("node %s", "节点 %s"), v.run.node);
    else if (v.run.running) snprintf(t, sizeof(t), L("node -", "节点 -"));
    else snprintf(t, sizeof(t), L("no job", "无任务"));
    setLbl(c.node, t);
    float pct = 0;
    if (v.run.pmax > 0) {
      pct = 100.0f * v.run.pv / v.run.pmax;
      snprintf(t, sizeof(t), "%d/%d %d%%", (int)v.run.pv, (int)v.run.pmax, (int)pct);
    } else if (v.run.running && v.nodeTotal > 0) {
      int den = v.nodeTotal - v.nodesCached; if (den < 1) den = 1;
      pct = 100.0f * v.nodesDone / den;
      snprintf(t, sizeof(t), L("nodes %d/%d", "节点 %d/%d"), v.nodesDone, den);
    } else strcpy(t, "");
    setLbl(c.step, t);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    setBar(c.bar, (int)pct, C_OK, nullptr, "");
    if (c.vramVal) {
      if (v.vitals.ok && v.vitals.vram_total > 0) {
        double used = v.vitals.vram_total - v.vitals.vram_free;
        if (used < 0) used = 0;
        float p = 100.0 * used / v.vitals.vram_total;
        snprintf(t, sizeof(t), "%.1f/%.0fG", used / 1073741824.0, v.vitals.vram_total / 1073741824.0);
        setBar(c.vbar, (int)(p + 0.5f), C_ACCENT, c.vramVal, t);
      } else setBar(c.vbar, 0, C_ACCENT, c.vramVal, "--");
    }
    if (c.ramVal) {
      if (v.vitals.ok && v.vitals.ram_total > 0) {
        double used = v.vitals.ram_total - v.vitals.ram_free;
        if (used < 0) used = 0;
        float p = 100.0 * used / v.vitals.ram_total;
        snprintf(t, sizeof(t), "%.1f/%.0fG", used / 1073741824.0, v.vitals.ram_total / 1073741824.0);
        setBar(c.rbar, (int)(p + 0.5f), C_ACCENT, c.ramVal, t);
      } else setBar(c.rbar, 0, C_ACCENT, c.ramVal, "--");
    }
    if (c.gpu) {
      setLbl(c.gpu, v.vitals.ok ? v.vitals.gpu : "--");
      setGpuMark(c.img, v.vitals.ok ? v.vitals.gpuVendor : 0);
      setLbl(c.acc, v.vitals.ok ? v.vitals.accel : "");
    }
    if (c.ver) {
      if (!v.vitals.ok) setLbl(c.ver, v.serverOk ? L("connected", "已连接") : (stillLinking(v.everSeen) ? L("connecting...", "连接中…") : L("offline", "离线")));
      else snprintf(t, sizeof(t), "ComfyUI v%s", v.vitals.version);
      setLbl(c.ver, t);
    }
    if (c.queue) {
      if (v.serverOk) snprintf(t, sizeof(t), L("queue %d", "队列 %d"), v.run.pending);
      else strcpy(t, L("queue -", "队列 -"));
      setLbl(c.queue, t);
    }
  }
}

static lv_color_t pctColor(int p) {
  if (p < 0) return C_MUT;
  if (p < 60) return C_OK;
  if (p < 85) return C_WARN;
  return C_ERR;
}
// Ultra overview cell. Tall cells (1/2/3 hosts) get full metric bars; short 2x2
// cells (4 hosts) collapse the four metrics into a compact percent grid.
static void buildUltraCell(int h, lv_obj_t *tile, int x, int y, int cw, int ch) {
  UltraCell &c = ucell[h];
  lv_obj_t *card = mkCard(tile, x, y, cw, ch);
  int iw = cw - 12, ih = ch - 12;
  c.compact = (ih < 200);
  static const char *MLBL[4] = {"CPU", "GPU", "RAM", "VRAM"};
  int cy = 0;
  c.title = mkLabel(card, cellTitle(h), C_MUT, 0, cy, 12);
  lv_obj_set_width(c.title, iw - 44);
  lv_label_set_long_mode(c.title, LV_LABEL_LONG_DOT);
  c.queue = mkLabel(card, "", C_MUT, 0, cy, 12);
  lv_obj_align(c.queue, LV_ALIGN_TOP_RIGHT, 0, cy);
  cy += 16;
  int sf = (!c.compact && iw >= 200) ? 20 : 16;
  c.state = mkLabel(card, "--", C_ACCENT, 0, cy, sf);
  c.dur = mkLabel(card, "00:00", C_TXT, 0, cy, sf);
  lv_obj_align(c.dur, LV_ALIGN_TOP_RIGHT, 0, cy);
  cy += sf + 4;
  if (!c.compact) {
    c.node = mkLabel(card, "", C_MUT, 0, cy, 12);
    lv_obj_set_width(c.node, iw - 56);
    lv_label_set_long_mode(c.node, LV_LABEL_LONG_DOT);
    c.step = mkLabel(card, "", C_TXT, 0, cy, 12);
    lv_obj_align(c.step, LV_ALIGN_TOP_RIGHT, 0, cy);
    cy += 18;
  }
  c.bar = mkBarIn(card, 0, cy, iw, 10, C_OK);
  cy += 16;
  if (!c.compact) {
    c.img = lv_img_create(card);
    lv_obj_set_pos(c.img, 0, cy - 1);
    c.gpu = mkLabel(card, "--", C_TXT, 20, cy, 12);
    lv_obj_set_width(c.gpu, iw - 20);
    lv_label_set_long_mode(c.gpu, LV_LABEL_LONG_DOT);
    cy += 18;
    for (int k = 0; k < 4; k++) {
      c.mLbl[k] = mkLabel(card, MLBL[k], C_MUT, 0, cy, 12);
      c.mVal[k] = mkLabel(card, "--", C_TXT, 0, cy, 12);
      lv_obj_align(c.mVal[k], LV_ALIGN_TOP_RIGHT, 0, cy);
      c.mBar[k] = mkBarIn(card, 0, cy + 15, iw, 7, C_ACCENT);
      cy += 27;
    }
  } else {
    int tw = (iw - 6) / 2;
    for (int k = 0; k < 4; k++) {
      int tx = (k % 2) * (tw + 6);
      int ty = cy + (k / 2) * 26;
      lv_obj_t *t = mkCard(card, tx, ty, tw, 24, C_INNER);
      c.mLbl[k] = mkLabel(t, MLBL[k], C_MUT, 0, 3, 12);
      c.mVal[k] = mkLabel(t, "--", C_TXT, 0, 3, 12);
      lv_obj_align(c.mVal[k], LV_ALIGN_TOP_RIGHT, 0, 3);
    }
    cy += 52;
  }
  c.power = mkLabel(card, "", C_MUT, 0, cy, 12);
  lv_obj_set_width(c.power, iw);
  lv_label_set_long_mode(c.power, LV_LABEL_LONG_DOT);
}
void refreshUltraDash() {
  char t[72], st[16];
  for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) {
    UltraCell &c = ucell[h];
    if (!c.state) continue;
    const HostView &v = dview[h];
    const Ultra &u = v.ultra;
    lv_color_t col;
    hostStateText(v, st, sizeof(st), col);
    setLbl(c.state, st);
    lv_obj_set_style_text_color(c.state, col, 0);
    uint32_t secs = (v.serverOk && v.run.running && v.run.start_ms) ? (millis() - v.run.start_ms) / 1000 : 0;
    if (v.serverOk) snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
    else strcpy(t, "--:--");
    setLbl(c.dur, t);
    if (c.node) {
      if (!v.serverOk) snprintf(t, sizeof(t), "%s", stillLinking(v.everSeen) ? L("waiting...", "等待连接…") : L("no response", "无响应"));
      else if (v.run.running && v.run.node[0]) snprintf(t, sizeof(t), L("node %s", "节点 %s"), v.run.node);
      else if (v.run.running) snprintf(t, sizeof(t), L("node -", "节点 -"));
      else snprintf(t, sizeof(t), L("no job", "无任务"));
      setLbl(c.node, t);
    }
    float pct = 0;
    if (v.run.pmax > 0) {
      pct = 100.0f * v.run.pv / v.run.pmax;
      snprintf(t, sizeof(t), "%d/%d %d%%", (int)v.run.pv, (int)v.run.pmax, (int)pct);
    } else if (v.run.running && v.nodeTotal > 0) {
      int den = v.nodeTotal - v.nodesCached; if (den < 1) den = 1;
      pct = 100.0f * v.nodesDone / den;
      snprintf(t, sizeof(t), L("nodes %d/%d", "节点 %d/%d"), v.nodesDone, den);
    } else strcpy(t, "");
    if (c.step) setLbl(c.step, t);
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    setBar(c.bar, (int)pct, C_OK, nullptr, "");
    if (c.queue) {
      if (v.serverOk) snprintf(t, sizeof(t), L("Q%d", "队%d"), v.run.pending);
      else strcpy(t, L("Q-", "队-"));
      setLbl(c.queue, t);
    }
    if (c.gpu) {
      setLbl(c.gpu, v.vitals.ok ? v.vitals.gpu : (u.ok ? u.gpuName : "--"));
      setGpuMark(c.img, v.vitals.ok ? v.vitals.gpuVendor : u.gpuVendor);
    }
    int vals[4] = {u.cpu, u.gpu, u.ram, u.vram};
    for (int k = 0; k < 4; k++) {
      if (!c.mVal[k]) continue;
      int p = vals[k];
      bool unknown = !u.ok || p < 0;
      if (unknown) strcpy(t, "--"); else snprintf(t, sizeof(t), "%d%%", p);
      setLbl(c.mVal[k], t);
      lv_obj_set_style_text_color(c.mVal[k], unknown ? C_MUT : pctColor(p), 0);
      if (c.mBar[k]) setBar(c.mBar[k], unknown ? 0 : p, pctColor(p), nullptr, "");
    }
    if (c.power) {
      if (!u.ok) snprintf(t, sizeof(t), "%s", u.everSeen ? L("power: assistant offline", "功耗：助手离线") : L("power: connecting", "功耗：连接中"));
      else if (u.watts < 0) snprintf(t, sizeof(t), "%s", L("power --", "功耗 --"));
      else snprintf(t, sizeof(t), L("power %.0f W", "功耗 %.0f W"), u.watts);
      setLbl(c.power, t);
      lv_obj_set_style_text_color(c.power, (u.ok && u.watts >= 0) ? C_WARN : C_MUT, 0);
    }
  }
}
void refreshGridLogs() {
  char st[16];
  for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) {
    LogCell &c = glog[h];
    if (!c.log) continue;
    const HostView &v = dview[h];
    lv_color_t col;
    setLbl(c.state, hostStateShort(v, col));
    lv_obj_set_style_text_color(c.state, col, 0);
    lockNet();
    if (dviewLogVer[h] != net[h].logVer) {
      dviewLogVer[h] = net[h].logVer;
      int shown = net[h].logCount;
      if (shown > c.lines) shown = c.lines;
      int start = net[h].logCount - shown;
      static char buf[LOG_LINES * 52];
      buf[0] = 0;
      for (int i = 0; i < shown; i++) {
        strlcat(buf, net[h].logLines[start + i], sizeof(buf));
        if (i < shown - 1) strlcat(buf, "\n", sizeof(buf));
      }
      unlockNet();
      setLbl(c.log, buf);
    } else {
      unlockNet();
    }
  }
}

void refreshGridOut() {
  for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) {
    OutCell &c = gout[h];
    if (!c.title) continue;
    const HostView &v = dview[h];
    for (int k = 0; k < c.jobs; k++) {
      if (!c.name[k]) continue;
      if (k < v.nJobs) {
        char nm[20], info[220];
        jobInfo(v.jobs[k], true, nm, sizeof(nm), info, sizeof(info));
        setLbl(c.name[k], nm);
        setLbl(c.meta[k], info);
        bool has = false;
        uint8_t tstate = 0;
        lockNet();
        NetData &nd = net[h];
        tstate = nd.thumbState[k];
        has = (tstate == 1);
        if (dviewThumbVer[h][k] != nd.thumbVer[k]) {
          dviewThumbVer[h][k] = nd.thumbVer[k];
          lv_img_dsc_t *img = (lv_img_dsc_t *)lv_canvas_get_img(c.cv[k]);
          if (img && img->data && nd.thumbs[k] && tstate == 1) {
            memcpy((void *)img->data, nd.thumbs[k], THUMB_W * THUMB_H * 2);
            has = true;
          }
        }
        unlockNet();
        if (has) {
          lv_obj_clear_flag(c.cv[k], LV_OBJ_FLAG_HIDDEN);
          lv_obj_add_flag(c.ph[k], LV_OBJ_FLAG_HIDDEN);
          lv_obj_invalidate(c.cv[k]);
        } else {
          lv_obj_add_flag(c.cv[k], LV_OBJ_FLAG_HIDDEN);
          lv_obj_clear_flag(c.ph[k], LV_OBJ_FLAG_HIDDEN);
          setLbl(c.ph[k], tstate == 3 ? L("no preview", "无预览") : (tstate == 2 ? L("failed", "失败") : L("loading...", "加载中…")));
        }
      } else {
        setLbl(c.name[k], "--");
        setLbl(c.meta[k], "");
        lv_obj_add_flag(c.cv[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(c.ph[k], LV_OBJ_FLAG_HIDDEN);
        setLbl(c.ph[k], L("empty", "空"));
      }
    }
  }
}

uint32_t lastDashBuild = 0;
void buildDashboard() {
  if (nHosts == 0) return;
  lastDashBuild = millis();
  if (scrDash) {
    if (scrSetup && lv_scr_act() == scrDash) lv_scr_load(scrSetup);
    // canvas buffers are ours (PSRAM), LVGL only frees the widget
    for (int h = 0; h < MAX_HOSTS; h++) {
      lv_obj_t *cvs[MAX_JOBS * 2];
      int nc = 0;
      for (int k = 0; k < MAX_JOBS; k++) {
        if (oui[h].canvas[k]) cvs[nc++] = oui[h].canvas[k];
        if (gout[h].cv[k]) cvs[nc++] = gout[h].cv[k];
      }
      for (int k = 0; k < nc; k++) {
        const lv_img_dsc_t *d = lv_canvas_get_img(cvs[k]);
        if (d && d->data) free((void *)d->data);
      }
    }
    lv_obj_del(scrDash);
    scrDash = nullptr;
  }
  for (int i = 0; i < MAX_HOSTS; i++) {
    mui[i] = MonUI(); oui[i] = OutUI();
    gdash[i] = DashCell(); glog[i] = LogCell(); gout[i] = OutCell(); ucell[i] = UltraCell();
  }
  for (int i = 0; i < MAX_HOSTS; i++) {
    lastVer[i] = 0xFFFFFFFFu;
    lastLogVer[i] = 0xFFFFFFFFu;
    for (int k = 0; k < MAX_JOBS; k++) lastThumbVer[i][k] = 0xFFFFFFFFu;
  }

  scrDash = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scrDash, C_BG, 0);
  lv_obj_set_style_border_width(scrDash, 0, 0);
  lv_obj_clear_flag(scrDash, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_pad_all(scrDash, 0, 0);

  lv_obj_t *bar = mkCard(scrDash, 0, 0, UI_W, 34, C_HEAD);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_t *ttl = mkLabel(bar, "ComfyUI", C_ACCENT, 12, 7, 16);
  lv_obj_align(ttl, LV_ALIGN_LEFT_MID, 12, 0);
  dotServer = lv_obj_create(bar);
  lv_obj_set_size(dotServer, 8, 8);
  lv_obj_set_style_radius(dotServer, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dotServer, C_ERR, 0);
  lv_obj_set_style_border_width(dotServer, 0, 0);
  lv_obj_align(dotServer, LV_ALIGN_LEFT_MID, 84, -1);
  lblHost = mkLabel(bar, "--", C_MUT, 0, 0, 12);
  lv_label_set_long_mode(lblHost, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lblHost, 110);
  lv_obj_align(lblHost, LV_ALIGN_LEFT_MID, 98, 0);
  lv_obj_t *divider = lv_obj_create(scrDash);
  lv_obj_set_size(divider, UI_W, 1);
  lv_obj_set_pos(divider, 0, 33);
  lv_obj_set_style_bg_color(divider, C_LINE, 0);
  lv_obj_set_style_border_width(divider, 0, 0);
  lv_obj_set_style_radius(divider, 0, 0);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < 6; i++) {
    pageDots[i] = lv_obj_create(bar);
    lv_obj_set_style_radius(pageDots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(pageDots[i], 0, 0);
    lv_obj_set_style_bg_color(pageDots[i], C_MUT, 0);
    lv_obj_set_size(pageDots[i], 6, 6);
    lv_obj_set_pos(pageDots[i], 236 + i * 12, 14);
    lv_obj_add_flag(pageDots[i], LV_OBJ_FLAG_HIDDEN);
  }
  lblPageSub = mkLabel(bar, "MON", C_MUT, 300, 10, 12);
  lblClock = mkLabel(bar, "--:--:--", C_TXT, 0, 0, 14);
  lv_obj_align(lblClock, LV_ALIGN_RIGHT_MID, -44, 0);
  for (int i = 0; i < 4; i++) {
    wifiBars[i] = lv_obj_create(bar);
    int h = 4 + i * 2;
    lv_obj_set_size(wifiBars[i], 3, h);
    lv_obj_set_pos(wifiBars[i], 341 + i * 4, 15 - h);
    lv_obj_set_style_radius(wifiBars[i], 1, 0);
    lv_obj_set_style_bg_color(wifiBars[i], C_LINE, 0);
    lv_obj_set_style_border_width(wifiBars[i], 0, 0);
  }
  lv_obj_t *gear = lv_btn_create(bar);
  lv_obj_set_size(gear, 28, 24);
  lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -6, 0);
  lv_obj_set_style_bg_color(gear, C_CARD, 0);
  lv_obj_set_style_shadow_width(gear, 0, 0);
  lv_obj_t *gl = lv_label_create(gear);
  lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
  lv_obj_center(gl);
  lv_obj_add_event_cb(gear, [](lv_event_t *) { showSetupTab(0); updateWifiStatus(); }, LV_EVENT_CLICKED, NULL);

  tileView = lv_tileview_create(scrDash);
  lv_obj_set_pos(tileView, 0, 34);
  lv_obj_set_size(tileView, UI_W, 286);
  lv_obj_set_style_bg_color(tileView, C_BG, 0);
  lv_obj_set_style_border_width(tileView, 0, 0);
  lv_obj_set_style_pad_all(tileView, 0, 0);
  lv_obj_set_scrollbar_mode(tileView, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_anim_time(tileView, 150, 0);

  if (useGrid()) {
    int C = gridCols(), R = gridRows();
    int cw = (UI_W - 8 - (C - 1) * 4) / C;
    int ch = (286 - 8 - (R - 1) * 4) / R;
    for (int pg = 0; pg < 3; pg++) {
      lv_obj_t *t1 = lv_tileview_add_tile(tileView, pg, 0, LV_DIR_HOR);
      lv_obj_set_style_bg_color(t1, C_BG, 0);
      lv_obj_set_style_border_width(t1, 0, 0);
      lv_obj_set_style_pad_all(t1, 0, 0);
      for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) {
        int x = 4 + (h % C) * (cw + 4);
        int y = 4 + (h / C) * (ch + 4);
        if (pg == 0) { if (ultraEnabled) buildUltraCell(h, t1, x, y, cw, ch); else buildDashCell(h, t1, x, y, cw, ch); }
        else if (pg == 1) buildLogsCell(h, t1, x, y, cw, ch);
        else buildOutCell(h, t1, x, y, cw, ch);
      }
    }
  } else {
    lv_obj_t *t1 = lv_tileview_add_tile(tileView, 0, 0, LV_DIR_HOR);
    lv_obj_set_style_bg_color(t1, C_BG, 0);
    lv_obj_set_style_border_width(t1, 0, 0);
    lv_obj_set_style_pad_all(t1, 0, 0);
    buildMonitor(0, t1);
    lv_obj_t *t2 = lv_tileview_add_tile(tileView, 1, 0, LV_DIR_HOR);
    lv_obj_set_style_bg_color(t2, C_BG, 0);
    lv_obj_set_style_border_width(t2, 0, 0);
    lv_obj_set_style_pad_all(t2, 0, 0);
    buildOutputs(0, t2);
  }
  lv_obj_add_event_cb(tileView, [](lv_event_t *) {
    if (millis() - lastDashBuild < 1000) return;
    lv_obj_t *act = lv_tileview_get_tile_act(tileView);
    if (!act) return;
    int idx = (int)lv_obj_get_index(act);
    if (idx >= 0 && idx < pageCount() && idx != activePageIdx) switchPage(idx);
  }, LV_EVENT_VALUE_CHANGED, NULL);

  activeHost = 0; activePage = 0; activePageIdx = 0;
  netActive = 0;
  lv_obj_set_tile_id(tileView, 0, 0, LV_ANIM_OFF);
  lockNet();
  for (int i = 0; i < MAX_HOSTS; i++) net[i].wantThumbs = false;
  if (!useGrid()) net[0].wantThumbs = true;
  unlockNet();
  for (int i = 0; i < MAX_HOSTS; i++) {
    dviewVer[i] = 0xFFFFFFFFu;
    dviewLogVer[i] = 0xFFFFFFFFu;
    for (int k = 0; k < MAX_JOBS; k++) dviewThumbVer[i][k] = 0xFFFFFFFFu;
  }
  updatePageDots();
  refreshHeader();
  refreshClock();
}

// ================= UI refresh =================
void refreshMonitor() {
  MonUI &m = mui[activeHost];
  if (!m.lblState) return;
  char t[64];
  const char *s;
  lv_color_t c;
  if (wst != W_ONLINE) { s = L("WIFI?", "无网络"); c = C_ERR; }
  else if (!serverOk) {
    bool linking = stillLinking(everSeen);
    s = linking ? L("CONNECTING", "连接中") : L("OFFLINE", "离线");
    c = linking ? C_WARN : C_ERR;
  }
  else if (run.running) { s = L("GENERATING", "生成中"); c = C_OK; }
  else if (run.pending > 0) { s = L("QUEUED", "排队中"); c = C_WARN; }
  else { s = L("IDLE", "空闲"); c = C_ACCENT; }
  setLbl(m.lblState, s);
  lv_obj_set_style_text_color(m.lblState, c, 0);

  uint32_t secs = (serverOk && run.running && run.start_ms) ? (millis() - run.start_ms) / 1000 : 0;
  if (serverOk) snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
  else strcpy(t, "--:--");
  setLbl(m.lblDur, t);

  if (!serverOk) snprintf(t, sizeof(t), "%s", stillLinking(everSeen) ? L("waiting...", "等待连接…") : L("no response", "无响应"));
  else if (run.running && strlen(run.node)) snprintf(t, sizeof(t), L("node %s", "节点 %s"), run.node);
  else if (run.running) snprintf(t, sizeof(t), L("node -", "节点 -"));
  else snprintf(t, sizeof(t), L("no job running", "无任务"));
  setLbl(m.lblNode, t);
  if (serverOk) snprintf(t, sizeof(t), L("queue %d", "队列 %d"), run.pending);
  else strcpy(t, L("queue -", "队列 -"));
  setLbl(m.lblQueue, t);

  bool haveProg = run.running && run.pmax > 0;
  bool haveNodes = !haveProg && run.running && nodeTotal > 0;
  if (haveNodes) {
    int denom = nodeTotal - nodesCached;
    if (denom < 1) denom = 1;
    int pct = 100 * nodesDone / denom;
    if (pct > 100) pct = 100;
    if (lv_bar_get_value(m.barProg) != pct) lv_bar_set_value(m.barProg, pct, LV_ANIM_OFF);
    lv_obj_clear_flag(m.barProg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m.lblStep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m.lblHint, LV_OBJ_FLAG_HIDDEN);
    snprintf(t, sizeof(t), L("nodes %d/%d  %d%%", "节点 %d/%d  %d%%"), nodesDone, denom, pct);
    setLbl(m.lblStep, t);
  } else if (haveProg) {
    float pr = 100.0f * run.pv / run.pmax;
    if (lv_bar_get_value(m.barProg) != (int)pr) lv_bar_set_value(m.barProg, (int)pr, LV_ANIM_OFF);
    lv_obj_clear_flag(m.barProg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m.spinner, LV_OBJ_FLAG_HIDDEN);
    if (run.stepSec > 0) snprintf(t, sizeof(t), L("step %d/%d  %d%%  %.1fs", "步 %d/%d  %d%%  %.1fs"), (int)run.pv, (int)run.pmax, (int)pr, run.stepSec);
    else snprintf(t, sizeof(t), L("step %d/%d  %d%%", "步 %d/%d  %d%%"), (int)run.pv, (int)run.pmax, (int)pr);
    setLbl(m.lblStep, t);
    lv_obj_clear_flag(m.lblStep, LV_OBJ_FLAG_HIDDEN);
    setLbl(m.lblHint, "");
    lv_obj_add_flag(m.lblHint, LV_OBJ_FLAG_HIDDEN);
  } else if (!serverOk) {
    lv_obj_add_flag(m.barProg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m.lblStep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m.lblHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(m.lblHint, 10, 45);
    setLbl(m.lblHint, stillLinking(everSeen) ? L("connecting to host...", "正在连接主机…")
                                             : L("host offline - check power / ip:port", "主机离线 - 检查电源 / IP:端口"));
  } else if (run.running) {
    lv_obj_add_flag(m.barProg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m.lblStep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m.lblHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(m.lblHint, 32, 45);
    if (secs > 5) setLbl(m.lblHint, L("no sampler progress - this workflow does not emit it", "该工作流不发送采样进度"));
    else setLbl(m.lblHint, L("waiting for sampler progress...", "等待采样进度…"));
  } else {
    lv_obj_add_flag(m.barProg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m.spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(m.lblStep, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(m.lblHint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(m.lblHint, 10, 45);
    setLbl(m.lblHint, L("idle - logs above update while a job runs", "空闲 - 任务运行时更新日志"));
  }

  if (ultraEnabled) {
    int vals[4] = {ultra.cpu, ultra.gpu, ultra.ram, ultra.vram};
    for (int k = 0; k < 4; k++) {
      if (!m.uVal[k]) continue;
      int p = vals[k];
      bool unknown = !ultra.ok || p < 0;
      if (unknown) strcpy(t, "--"); else snprintf(t, sizeof(t), "%d%%", p);
      setLbl(m.uVal[k], t);
      lv_obj_set_style_text_color(m.uVal[k], unknown ? C_MUT : pctColor(p), 0);
      setBar(m.uBar[k], unknown ? 0 : p, pctColor(p), nullptr, "");
    }
    if (m.uPower) {
      if (!ultra.ok) snprintf(t, sizeof(t), "%s", ultra.everSeen ? L("power: assistant offline", "功耗：助手离线") : L("power: connecting", "功耗：连接中"));
      else if (ultra.watts < 0) snprintf(t, sizeof(t), "%s", L("power --", "功耗 --"));
      else snprintf(t, sizeof(t), L("power %.0f W", "功耗 %.0f W"), ultra.watts);
      setLbl(m.uPower, t);
      lv_obj_set_style_text_color(m.uPower, (ultra.ok && ultra.watts >= 0) ? C_WARN : C_MUT, 0);
    }
  } else if (vitals.ok && vitals.vram_total > 0) {
    double used = vitals.vram_total - vitals.vram_free;
    if (used < 0) used = 0;
    float pct = 100.0 * used / vitals.vram_total;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    snprintf(t, sizeof(t), "%.1f/%.0fG", used / 1073741824.0, vitals.vram_total / 1073741824.0);
    setBar(m.gBar[0], (int)(pct + 0.5), pct > 88 ? C_ERR : (pct > 70 ? C_WARN : C_ACCENT), m.gVal[0], t);
  } else setBar(m.gBar[0], 0, C_MUT, m.gVal[0], "--");
  if (!ultraEnabled) {
    if (vitals.ok && vitals.ram_total > 0) {
      double used = vitals.ram_total - vitals.ram_free;
      if (used < 0) used = 0;
      float pct = 100.0 * used / vitals.ram_total;
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
      snprintf(t, sizeof(t), "%.1f/%.0fG", used / 1073741824.0, vitals.ram_total / 1073741824.0);
      setBar(m.gBar[1], (int)(pct + 0.5), pct > 85 ? C_WARN : C_ACCENT, m.gVal[1], t);
    } else setBar(m.gBar[1], 0, C_MUT, m.gVal[1], "--");
  }
  if (vitals.ok) {
    setLbl(m.lblGpu, vitals.gpu);
    setGpuMark(m.imgGpu, vitals.gpuVendor);
    setLbl(m.lblAcc, vitals.accel);
    snprintf(t, sizeof(t), "ComfyUI v%s", vitals.version);
    setLbl(m.lblVer, t);
  } else {
    setLbl(m.lblGpu, "--");
    setGpuMark(m.imgGpu, 0);
    setLbl(m.lblAcc, "");
    setLbl(m.lblVer, serverOk ? L("connected", "已连接") : (stillLinking(everSeen) ? L("connecting...", "连接中…") : L("offline", "离线")));
  }

  // log (copy under lock)
  char logBuf[LOG_LINES * 52];
  logBuf[0] = 0;
  bool logNew = false;
  lockNet();
  if (lastLogVer[activeHost] != net[activeHost].logVer) {
    lastLogVer[activeHost] = net[activeHost].logVer;
    int shown = net[activeHost].logCount > 9 ? 9 : net[activeHost].logCount;
    int start = net[activeHost].logCount - shown;
    for (int i = 0; i < shown; i++) {
      strcat(logBuf, net[activeHost].logLines[start + i]);
      if (i < shown - 1) strcat(logBuf, "\n");
    }
    logNew = true;
  }
  unlockNet();
  if (logNew) setLbl(m.logLabel, logBuf);
}

static void jobInfo(const Job &j, bool compact, char *name, size_t nn, char *info, size_t ni) {
  const char *fn = j.file;
  size_t fl = strlen(fn);
  if (fl > 14) snprintf(name, nn, "..%s", fn + fl - 12);
  else snprintf(name, nn, "%s", fn);
  char line[80];
  info[0] = 0;
  if (j.width && j.height) snprintf(line, sizeof(line), "%ux%u", j.width, j.height);
  else strcpy(line, "--");
  if (j.videoSec > 0.05f) {
    size_t l = strlen(line);
    snprintf(line + l, sizeof(line) - l, " %.1fs", j.videoSec);
  }
  strcat(info, line);
  strcat(info, "\n");
  if (j.videoSec > 0.05f) {
    if (j.sizeKnown) snprintf(line, sizeof(line), "%.1fMbps", j.sizeBytes * 8.0 / 1e6 / j.videoSec);
    else strcpy(line, "--Mbps");
    if (j.fps > 0) {
      size_t l = strlen(line);
      snprintf(line + l, sizeof(line) - l, " %.0ffps", j.fps);
    }
  } else {
    snprintf(line, sizeof(line), L("image", "图片"));
  }
  strcat(info, line);
  strcat(info, "\n");
  float genSec = j.dur_ms / 1000.0f;
  if (j.videoSec > 0.05f) snprintf(line, sizeof(line), L("gen %.0fs %.0fs/s", "耗时%.0fs %.0fs/s"), genSec, genSec / j.videoSec);
  else snprintf(line, sizeof(line), L("gen %.0fs", "耗时%.0fs"), genSec);
  strcat(info, line);
  strcat(info, "\n");
  if (j.end_epoch > 1700000000) {
    struct tm tmv; time_t e = j.end_epoch; localtime_r(&e, &tmv);
    if (compact) snprintf(line, sizeof(line), "%02d:%02d %s", tmv.tm_hour, tmv.tm_min, j.ok ? L("ok", "成功") : L("ERR", "失败"));
    else snprintf(line, sizeof(line), "%02d:%02d:%02d %s", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, j.ok ? L("ok", "成功") : L("ERR", "失败"));
  } else snprintf(line, sizeof(line), "%s", j.ok ? L("done", "完成") : L("FAILED", "失败"));
  strcat(info, line);
}

void refreshOutputs() {
  OutUI &o = oui[activeHost];
  NetData &nd = net[activeHost];
  for (int k = 0; k < MAX_JOBS; k++) {
    if (!o.name[k]) continue;
    if (k < nJobs) {
      char nm[20], info[220];
      jobInfo(jobs[k], false, nm, sizeof(nm), info, sizeof(info));
      setLbl(o.name[k], nm);
      setLbl(o.meta[k], info);
      bool has;
      uint8_t tstate;
      lockNet();
      tstate = nd.thumbState[k];
      has = (nd.thumbVer[k] != 0xFFFFFFFFu) && (tstate == 1);
      bool newThumb = (lastThumbVer[activeHost][k] != nd.thumbVer[k]);
      if (newThumb) {
        lastThumbVer[activeHost][k] = nd.thumbVer[k];
        lv_img_dsc_t *img = (lv_img_dsc_t *)lv_canvas_get_img(o.canvas[k]);
        if (img && img->data && nd.thumbs[k]) {
          memcpy((void *)img->data, nd.thumbs[k], THUMB_W * THUMB_H * 2);
          has = true;
        }
      }
      unlockNet();
      if (newThumb && has) lv_obj_invalidate(o.canvas[k]);
      if (has) {
        lv_obj_clear_flag(o.canvas[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(o.ph[k], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(o.canvas[k], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(o.ph[k], LV_OBJ_FLAG_HIDDEN);
        if (tstate == 3) setLbl(o.ph[k], L("video/no preview", "视频/无预览"));
        else if (tstate == 2) setLbl(o.ph[k], L("preview failed", "预览失败"));
        else setLbl(o.ph[k], L("loading...", "加载中…"));
      }
    } else {
      setLbl(o.name[k], "--");
      setLbl(o.meta[k], "");
      lv_obj_add_flag(o.canvas[k], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(o.ph[k], LV_OBJ_FLAG_HIDDEN);
      setLbl(o.ph[k], L("empty", "空"));
    }
  }
}

// ================= pull / switch =================
bool pullActive() {
  bool changed = false;
  lockNet();
  NetData &nd = net[activeHost];
  if (nd.ver != lastVer[activeHost]) {
    vitals = nd.vitals;
    run = nd.run;
    ultra = nd.ultra;
    nJobs = nd.nJobs;
    for (int i = 0; i < nJobs; i++) jobs[i] = nd.jobs[i];
    serverOk = nd.serverOk;
    everSeen = nd.everSeen;
    nodeTotal = nd.nodeTotal;
    nodesDone = nd.nodesDone;
    nodesCached = nd.nodesCached;
    lastVer[activeHost] = nd.ver;
    changed = true;
  }
  unlockNet();
  return changed;
}
void switchPage(int idx) {
  if (idx < 0 || idx >= pageCount() || idx == activePageIdx) return;
  activePageIdx = idx;
  if (useGrid()) {
    activePage = idx;
    lockNet();
    for (int i = 0; i < MAX_HOSTS; i++) net[i].wantThumbs = (idx == 2);
    unlockNet();
    for (int h = 0; h < MAX_HOSTS; h++) {
      dviewLogVer[h] = 0xFFFFFFFFu;
      for (int k = 0; k < MAX_JOBS; k++) dviewThumbVer[h][k] = 0xFFFFFFFFu;
    }
    for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) pullHost(h);
    refreshHeader();
    if (idx == 0) { if (ultraEnabled) refreshUltraDash(); else refreshGridDash(); }
    else if (idx == 1) refreshGridLogs();
    else refreshGridOut();
    refreshClock();
    return;
  }
  activeHost = 0;
  netActive = 0;
  activePage = idx;
  lockNet();
  net[0].wantThumbs = true;
  unlockNet();
  refreshHeader();
  pullActive();
  if (idx == 0) refreshMonitor();
  else refreshOutputs();
  refreshClock();
}

// ================= HTTP / JSON =================
static const char *sp(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  return p;
}
static const char *keyVal(const char *s, const char *key) {
  char pat[48];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(s, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  p = sp(p);
  if (*p != ':') return nullptr;
  return sp(p + 1);
}
static double jnum(const char *s, const char *key, double def = 0) {
  const char *v = keyVal(s, key);
  return v ? strtod(v, nullptr) : def;
}
static void jstr(char *out, size_t n, const char *s, const char *key) {
  out[0] = 0;
  const char *v = keyVal(s, key);
  if (!v || *v != '"') return;
  v++;
  size_t i = 0;
  while (v[i] && i < n - 1) {
    if (v[i] == '\\' && (v[i + 1] == '"' || v[i + 1] == '\\')) { out[i] = v[i + 1]; i++; v += 2; continue; }
    if (v[i] == '"') break;
    out[i] = v[i];
    i++;
  }
  out[i] = 0;
}
static const char *findIn(const char *hay, size_t len, const char *needle, size_t from) {
  if (from >= len) return nullptr;
  size_t nl = strlen(needle);
  if (nl > len - from) return nullptr;
  for (size_t i = from; i + nl <= len; i++)
    if (hay[i] == needle[0] && memcmp(hay + i, needle, nl) == 0) return hay + i;
  return nullptr;
}
static double numIn(const char *base, size_t len, size_t from, const char *key) {
  const char *k = findIn(base, len, key, from);
  if (!k) return 0;
  const char *v = sp(k + strlen(key));
  if (*v != ':') return 0;
  return strtod(sp(v + 1), nullptr);
}
static int countArrElems(const char *base, size_t len, const char *key) {
  const char *k = findIn(base, len, key, 0);
  if (!k) return -1;
  const char *p = sp(k + strlen(key));
  if (*p != ':') return -1;
  p = sp(p + 1);
  if (*p != '[') return -1;
  int depth = 0, cnt = 0;
  bool instr = false, esc = false;
  for (; *p; p++) {
    char c = *p;
    if (instr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') instr = false; continue; }
    if (c == '"') { instr = true; continue; }
    if (c == '[' || c == '{') { depth++; if (depth == 2) cnt++; }
    else if (c == ']' || c == '}') { depth--; if (depth == 0) break; }
  }
  return cnt;
}
String hostUrl(const NetData &nd) { return "http://" + String(nd.host) + ":" + String(nd.port); }
bool httpGet(NetData &nd, const String &path, String &out) {
  HTTPClient c;
  c.setConnectTimeout(1200);
  c.setTimeout(4000);
  String url = hostUrl(nd) + path;
  if (!c.begin(url)) return false;
  int code = c.GET();
  bool ok = (code == 200);
  if (ok) out = c.getString();
  c.end();
  return ok;
}
bool httpGetBytes(NetData &nd, const String &path, uint8_t *buf, size_t cap, size_t &outLen) {
  HTTPClient c;
  c.setConnectTimeout(1500);
  c.setTimeout(8000);
  String url = hostUrl(nd) + path;
  if (!c.begin(url)) return false;
  int code = c.GET();
  if (code != 200) { c.end(); return false; }
  int len = c.getSize();
  if (len > 0 && (size_t)len > cap) { c.end(); return false; }
  WiFiClient *st = c.getStreamPtr();
  size_t got = 0;
  uint32_t t0 = millis();
  size_t limit = (len > 0) ? (size_t)len : cap;
  while (got < limit && millis() - t0 < 8000) {
    int avail = st->available();
    if (avail > 0) {
      size_t want = (size_t)avail;
      if (want > limit - got) want = limit - got;
      int n = st->read(buf + got, want);
      if (n > 0) got += n;
    } else {
      if (len <= 0 && !st->connected()) break;
      delay(2);
    }
  }
  c.end();
  outLen = got;
  return (len > 0) ? (got == (size_t)len) : (got > 100);
}
static const char *thumbSource(const Job &j) {
  if (isImageFile(j.file)) return j.file;
  if (j.poster[0] && isImageFile(j.poster)) return j.poster;
  return nullptr;
}
bool httpHeadSize(NetData &nd, const String &path, uint32_t &out) {
  HTTPClient c;
  c.setConnectTimeout(1200);
  c.setTimeout(4000);
  String url = hostUrl(nd) + path;
  if (!c.begin(url)) return false;
  int code = c.sendRequest("HEAD");
  int len = c.getSize();
  c.end();
  if (code == 200 && len > 0) { out = (uint32_t)len; return true; }
  return false;
}
static bool isVideoFile(const char *f) {
  const char *d = strrchr(f, '.');
  if (!d) return false;
  return !strcasecmp(d, ".mp4") || !strcasecmp(d, ".webm") || !strcasecmp(d, ".mov");
}
static bool isImageFile(const char *f) {
  const char *d = strrchr(f, '.');
  if (!d) return false;
  return !strcasecmp(d, ".png") || !strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg") || !strcasecmp(d, ".webp");
}
static String urlEnc(const char *s) {
  String o;
  for (const char *p = s; *p; p++) {
    char c = *p;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { char b[4]; snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

// ComfyUI only reports the accelerator through the pytorch build tag, e.g.
// "2.14.0+rocm10.1.0" -> "ROCm 10.1.0", "2.7.1+cu128" -> "CUDA 12.8".
static void accelFromTorch(const char *tv, char *out, size_t n) {
  out[0] = 0;
  const char *plus = strchr(tv, '+');
  if (!plus) return;
  char tag[24];
  size_t i = 0;
  for (const char *p = plus + 1; *p && i < sizeof(tag) - 1; p++) { tag[i] = (char)tolower((unsigned char)*p); i++; }
  tag[i] = 0;
  const char *name = nullptr;
  if (!strncmp(tag, "rocm", 4)) name = "ROCm";
  else if (!strncmp(tag, "cu", 2)) name = "CUDA";
  else if (!strncmp(tag, "mps", 3)) name = "MPS";
  else if (!strcmp(tag, "cpu")) name = "CPU";
  if (!name) return;
  const char *d = tag;
  while (*d && !isdigit((unsigned char)*d)) d++;
  if (!*d) { strlcpy(out, name, n); return; }
  char vb[16];
  if (name[0] == 'C' && name[1] == 'U' && name[2] == 'D' && strlen(d) == 3)
    snprintf(vb, sizeof(vb), "%c%c.%c", d[0], d[1], d[2]);
  else strlcpy(vb, d, sizeof(vb));
  snprintf(out, n, "%s %s", name, vb);
}

// "cuda:0 NVIDIA GeForce RTX 5070 : native" -> vendor 1, "RTX 5070"
// "cuda:0 AMD Radeon RX 7900 XT : native"   -> vendor 2, "RX 7900 XT"
static void gpuIdentify(const char *name, char *out, size_t n, uint8_t &vendor) {
  vendor = 0;
  out[0] = 0;
  char up[80];
  size_t i = 0;
  for (; name[i] && i < sizeof(up) - 1; i++) up[i] = (char)toupper((unsigned char)name[i]);
  up[i] = 0;
  const char *rtx = strstr(up, "RTX"), *gtx = strstr(up, "GTX"), *tesla = strstr(up, "TESLA");
  const char *rx = strstr(up, "RX"), *vega = strstr(up, "VEGA"), *adeon = strstr(up, "RADEON");
  if (rtx || gtx || tesla || strstr(up, "NVIDIA") || strstr(up, "GEFORCE")) vendor = 1;
  else if (rx || vega || adeon || strstr(up, "AMD")) vendor = 2;
  const char *start = nullptr;
  if (vendor == 1) start = rtx ? rtx : (gtx ? gtx : (tesla ? tesla : strstr(up, "GEFORCE")));
  else if (vendor == 2) start = rx ? rx : (vega ? vega : adeon);
  if (!start) {
    start = strchr(up, ' ');
    while (start && *start == ' ') start++;
  }
  if (!start) { strlcpy(out, up, n); return; }
  char tmp[32];
  size_t k = 0;
  for (const char *p = start; *p && k < sizeof(tmp) - 4; p++) {
    if (!strncmp(p, " : NATIVE", 9) || !strncmp(p, "GRAPHICS", 8) || !strncmp(p, "SERIES", 6)) break;
    tmp[k++] = *p;
  }
  tmp[k] = 0;
  // keep only the model tokens: drop marketing words after the number
  // ("RTX 5070 LAPTOP GPU" -> "RTX 5070", "RX 7900 XT" stays)
  bool seenDigit = false;
  char *gap = nullptr;
  for (char *q = tmp; *q; q++) {
    if (isdigit((unsigned char)*q)) seenDigit = true;
    if (seenDigit && *q == ' ') { gap = q; break; }
  }
  if (gap) {
    char *r = gap + 1;
    char *r2 = strchr(r, ' ');
    size_t rl = r2 ? (size_t)(r2 - r) : strlen(r);
    if (rl > 3) *gap = 0;
  }
  k = strlen(tmp);
  while (k && (tmp[k - 1] == ' ' || tmp[k - 1] == ':')) tmp[--k] = 0;
  strlcpy(out, tmp, n);
}

// ---------- worker fetchers ----------
void fetchVitals(NetData &nd) {
  String b;
  if (!httpGet(nd, "/system_stats", b)) {
    bool wentOff;
    lockNet(); wentOff = markOfflineLocked(nd, millis()); bump(nd); unlockNet();
    if (wentOff) logEvent(nd, L("server unreachable", "服务器不可达"));
    return;
  }
  Vitals v;
  const char *s = b.c_str();
  v.ram_total = jnum(s, "ram_total");
  v.ram_free = jnum(s, "ram_free");
  v.vram_total = jnum(s, "vram_total");
  v.vram_free = jnum(s, "vram_free");
  const char *dev = strstr(s, "\"devices\"");
  jstr(v.gpu_name, sizeof(v.gpu_name), dev ? dev : s, "name");
  gpuIdentify(v.gpu_name, v.gpu, sizeof(v.gpu), v.gpuVendor);
  jstr(v.version, sizeof(v.version), s, "comfyui_version");
  char tv[48];
  jstr(tv, sizeof(tv), s, "pytorch_version");
  accelFromTorch(tv, v.accel, sizeof(v.accel));
  v.ok = true;
  bool cameBack;
  lockNet();
  cameBack = !nd.serverOk;
  nd.vitals = v; markSeenLocked(nd); bump(nd);
  unlockNet();
  if (cameBack) logEvent(nd, L("server back", "服务器恢复"));
}
void fetchQueue(NetData &nd) {
  String b;
  if (!httpGet(nd, "/queue", b)) return;
  const char *s = b.c_str();
  size_t len = b.length();
  int pend = countArrElems(s, len, "\"queue_pending\"");
  int running = countArrElems(s, len, "\"queue_running\"");
  lockNet();
  if (pend >= 0) nd.run.pending = pend;
  if (running >= 0) {
    if (running > 0 && !nd.run.running) { nd.run.running = true; nd.run.start_ms = millis(); }
    if (running == 0 && nd.run.running && !nd.run.connected) { nd.run.running = false; nd.run.pv = nd.run.pmax = 0; nd.run.node[0] = 0; }
  }
  const char *qr = findIn(s, len, "\"queue_running\"", 0);
  if (qr && running > 0) {
    const char *qp = findIn(s, len, "\"queue_pending\"", (size_t)(qr - s));
    size_t end = qp ? (size_t)(qp - s) : len;
    int cnt = 0;
    const char *pp = qr;
    while (true) {
      const char *hit = findIn(s, end, "\"class_type\"", (size_t)(pp - s));
      if (!hit) break;
      cnt++;
      pp = hit + 12;
    }
    nd.nodeTotal = cnt;
    const char *cid = findIn(s, end, "\"client_id\"", (size_t)(qr - s));
    if (cid) {
      const char *v = sp(cid + strlen("\"client_id\""));
      if (*v == ':') {
        v = sp(v + 1);
        if (*v == '"') {
          v++;
          size_t k = 0;
          while (v[k] && v[k] != '"' && k < sizeof(nd.promptClient) - 1) { nd.promptClient[k] = v[k]; k++; }
          nd.promptClient[k] = 0;
        }
      }
    }
  } else if (running == 0) {
    nd.nodeTotal = 0;
    nd.nodesDone = 0;
    nd.nodesCached = 0;
  }
  markSeenLocked(nd); bump(nd);
  unlockNet();
}
// nullable number: -1 (def) when the key is absent or JSON null
static double jnumN(const char *s, const char *key, double def = -1) {
  const char *v = keyVal(s, key);
  if (!v) return def;
  v = sp(v);
  if (!strncmp(v, "null", 4)) return def;
  return strtod(v, nullptr);
}
// Ultra hardware vitals from the on-host assistant (port A). Independent of the
// 8188 serverOk flag: the assistant can be down while ComfyUI runs, or vice versa.
void fetchUltra(NetData &nd) {
  HTTPClient c;
  c.setConnectTimeout(1200);
  c.setTimeout(3000);
  String url = "http://" + String(nd.host) + ":" + String(ultraPort) + "/api/panel";
  String b;
  bool got = false;
  if (c.begin(url)) {
    int code = c.GET();
    if (code == 200) { b = c.getString(); got = true; }
    c.end();
  }
  lockNet();
  Ultra &u = nd.ultra;
  if (got) {
    const char *s = b.c_str();
    u.cpu = (int)jnumN(s, "cpu");
    u.ram = (int)jnumN(s, "ram");
    u.vram = (int)jnumN(s, "vram");
    u.watts = (float)jnumN(s, "w");
    u.cpuTemp = (float)jnumN(s, "cpu_temp");
    const char *g = strstr(s, "\"gpu\"");
    if (g) {
      double gu = jnumN(g, "util");
      u.gpu = gu < 0 ? -1 : (int)gu;
      u.gpuTemp = (float)jnumN(g, "temp");
      jstr(u.gpuName, sizeof(u.gpuName), g, "name");
      char vend[12];
      jstr(vend, sizeof(vend), g, "vendor");
      u.gpuVendor = !strcmp(vend, "nvidia") ? 1 : (!strcmp(vend, "amd") ? 2 : 0);
    }
    u.ok = true; u.everSeen = true; u.lastSeen = millis();
  } else if (u.ok && millis() - u.lastSeen > offlineMs()) {
    u.ok = false;
    u.cpu = u.gpu = u.ram = u.vram = -1;
    u.watts = u.gpuTemp = u.cpuTemp = -1;
  }
  bump(nd);
  unlockNet();
}
void fetchHistory(NetData &nd) {
  String b;
  if (!httpGet(nd, "/history?max_items=8", b)) return;
  lockNet(); markSeenLocked(nd); bump(nd); unlockNet();
  const char *s = b.c_str();
  size_t len = b.length();
  size_t starts[8];
  int n = 0;
  size_t p = 0;
  const char *needle = "\"prompt\":";
  while (n < 8) {
    const char *q = findIn(s, len, needle, p);
    if (!q) break;
    size_t off = q - s + strlen(needle);
    const char *c = sp(q + strlen(needle));
    if (*c == '[') { starts[n++] = q - s; p = off; }
    else p = off;
  }
  if (n == 0) return;
  Job tmp[MAX_JOBS];
  int tn = 0;
  for (int i = 0; i < n && tn < MAX_JOBS; i++) {
    size_t a = starts[i];
    size_t e = (i + 1 < n) ? starts[i + 1] : len;
    Job &j = tmp[tn];
    memset(&j, 0, sizeof(j));
    strcpy(j.type, "output");
    const char *fn = findIn(s, e, "\"filename\"", a);
    if (fn) {
      const char *v = sp(fn + strlen("\"filename\""));
      if (*v == ':') {
        v = sp(v + 1);
        if (*v == '"') {
          v++;
          size_t k = 0;
          while (v[k] && v[k] != '"' && k < sizeof(j.file) - 1) { j.file[k] = v[k]; k++; }
          j.file[k] = 0;
        }
      }
      const char *sf = findIn(s, e, "\"subfolder\"", (size_t)(fn - s));
      if (sf) {
        const char *v2 = sp(sf + strlen("\"subfolder\""));
        if (*v2 == ':') {
          v2 = sp(v2 + 1);
          if (*v2 == '"') { v2++; size_t k = 0; while (v2[k] && v2[k] != '"' && k < sizeof(j.subfolder) - 1) { j.subfolder[k] = v2[k]; k++; } j.subfolder[k] = 0; }
        }
      }
      const char *tp = findIn(s, e, "\"type\"", (size_t)(fn - s));
      if (tp) {
        const char *v2 = sp(tp + strlen("\"type\""));
        if (*v2 == ':') {
          v2 = sp(v2 + 1);
          if (*v2 == '"') { v2++; size_t k = 0; while (v2[k] && v2[k] != '"' && k < sizeof(j.type) - 1) { j.type[k] = v2[k]; k++; } j.type[k] = 0; }
        }
      }
      const char *wf = findIn(s, e, "\"workflow\"", (size_t)(fn - s));
      if (wf) {
        const char *v2 = sp(wf + strlen("\"workflow\""));
        if (*v2 == ':') {
          v2 = sp(v2 + 1);
          if (*v2 == '"') { v2++; size_t k = 0; while (v2[k] && v2[k] != '"' && k < sizeof(j.poster) - 1) { j.poster[k] = v2[k]; k++; } j.poster[k] = 0; }
        }
      }
      const char *md = findIn(s, e, "\"metadata\"", (size_t)(fn - s));
      if (md) {
        char meta[280];
        size_t mn = 0;
        const char *mp = md;
        while (*mp && mn < sizeof(meta) - 1 && mn < 260) meta[mn++] = *mp++;
        meta[mn] = 0;
        j.width = (uint16_t)jnum(meta, "width");
        j.height = (uint16_t)jnum(meta, "height");
        j.fps = (float)jnum(meta, "fps");
        j.videoSec = (float)jnum(meta, "duration");
        j.frames = (uint16_t)jnum(meta, "frame_count");
      }
    }
    const char *ss = findIn(s, e, "\"status_str\"", a);
    j.ok = true;
    if (ss) {
      const char *v = sp(sp(ss + 12) + 1);
      j.ok = strncmp(v, "\"success\"", 9) == 0;
    }
    if (!j.file[0] && j.ok) continue;
    if (!j.file[0]) strcpy(j.file, "(failed)");
    const char *es = findIn(s, e, "\"execution_start\"", a);
    const char *su = findIn(s, e, "\"execution_success\"", a);
    if (!su) su = findIn(s, e, "\"execution_error\"", a);
    double start_ts = es ? numIn(s, e, (size_t)(es - s), "\"timestamp\"") : 0;
    double succ_ts = su ? numIn(s, e, (size_t)(su - s), "\"timestamp\"") : 0;
    j.dur_ms = (succ_ts > start_ts) ? (uint32_t)(succ_ts - start_ts) : 0;
    j.end_epoch = (uint32_t)(succ_ts / 1000);
    tmp[tn++] = j;
  }
  lockNet();
  for (int i = 0; i < tn; i++) {
    for (int oi = 0; oi < nd.nJobs && oi < MAX_JOBS; oi++) {
      if (!strcmp(tmp[tn - 1 - i].file, nd.jobs[oi].file)) {
        tmp[tn - 1 - i].sizeBytes = nd.jobs[oi].sizeBytes;
        tmp[tn - 1 - i].sizeKnown = nd.jobs[oi].sizeKnown;
        tmp[tn - 1 - i].sizeAt = nd.jobs[oi].sizeAt;
        break;
      }
    }
  }
  nd.nJobs = tn;
  for (int i = 0; i < tn; i++) {
    nd.jobs[i] = tmp[tn - 1 - i];
    char sig[80];
    snprintf(sig, sizeof(sig), "%s|%s", nd.jobs[i].subfolder, nd.jobs[i].file);
    if (strcmp(sig, nd.thumbSig[i])) {
      strlcpy(nd.thumbSig[i], sig, sizeof(nd.thumbSig[i]));
      nd.thumbState[i] = 0;
      nd.thumbVer[i]++;
    }
  }
  bump(nd);
  unlockNet();
}

// ---------- thumbnails (worker) ----------
struct DrawCtx {
  uint16_t *buf;
  int dstW, dstH;
  float sx, sy;
  int ox, oy;
};
static DrawCtx *gDrawCtx = nullptr;
int jpegDraw(JPEGDRAW *pDraw) {
  DrawCtx *ctx = gDrawCtx;
  if (!ctx) return 1;
  for (int yy = 0; yy < pDraw->iHeight; yy++) {
    int sy = pDraw->y + yy;
    int dy = ctx->oy + (int)(sy * ctx->sy);
    if (dy < 0 || dy >= ctx->dstH) continue;
    for (int xx = 0; xx < pDraw->iWidth; xx++) {
      int sx = pDraw->x + xx;
      int dx = ctx->ox + (int)(sx * ctx->sx);
      if (dx < 0 || dx >= ctx->dstW) continue;
      ctx->buf[dy * ctx->dstW + dx] = pDraw->pPixels[yy * pDraw->iWidth + xx];
    }
  }
  return 1;
}
bool decodeJpeg(const uint8_t *jpg, size_t len, uint16_t *dst, int dw, int dh) {
  if (!jpg || len < 100 || !dst) return false;
  if (jpg[0] != 0xFF || jpg[1] != 0xD8) return false;
  DrawCtx ctx;
  ctx.buf = dst;
  ctx.dstW = dw;
  ctx.dstH = dh;
  for (int i = 0; i < dw * dh; i++) dst[i] = 0x0821;
  gDrawCtx = &ctx;
  if (!jpeg.openRAM((uint8_t *)jpg, (int)len, jpegDraw)) { gDrawCtx = nullptr; return false; }
  int scale = JPEG_SCALE_QUARTER;
  int iw = jpeg.getWidth() / 4, ih = jpeg.getHeight() / 4;
  if (ih > dh * 2 || iw > dw * 2) { scale = JPEG_SCALE_EIGHTH; iw = jpeg.getWidth() / 8; ih = jpeg.getHeight() / 8; }
  float s = fminf((float)dw / iw, (float)dh / ih);
  if (s > 1.0f) s = 1.0f;
  ctx.sx = s; ctx.sy = s;
  ctx.ox = (dw - (int)(iw * s)) / 2;
  ctx.oy = (dh - (int)(ih * s)) / 2;
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  int dec = jpeg.decode(0, 0, scale);
  jpeg.close();
  gDrawCtx = nullptr;
  return dec == 1;
}
void fetchJobThumb(NetData &nd, int k) {
  Job &j = nd.jobs[k];
  const char *src = thumbSource(j);
  if (!src) { nd.thumbState[k] = 3; return; }
  String path = "/view?filename=" + urlEnc(src) + "&type=" + urlEnc(j.type) +
                "&subfolder=" + urlEnc(j.subfolder) + "&preview=jpeg;quality=60";
  size_t got = 0;
  if (!httpGetBytes(nd, path, thumbJpg, THUMB_JPEG_MAX, got) || got < 100) {
    nd.thumbState[k] = 2;
    nd.thumbAt[k] = millis() + 120000;
    logEvent(nd, L("thumb fail #%d", "缩略图失败 #%d"), k + 1);
    return;
  }
  if (!decodeJpeg(thumbJpg, got, scrThumb, THUMB_W, THUMB_H)) {
    nd.thumbState[k] = 2;
    nd.thumbAt[k] = millis() + 120000;
    logEvent(nd, L("thumb decode fail", "缩略图解码失败"));
    return;
  }
  lockNet();
  if (!nd.thumbs[k]) nd.thumbs[k] = (uint16_t *)heap_caps_malloc(THUMB_W * THUMB_H * 2, MALLOC_CAP_SPIRAM);
  if (nd.thumbs[k]) {
    memcpy(nd.thumbs[k], scrThumb, THUMB_W * THUMB_H * 2);
    nd.thumbVer[k]++;
    nd.thumbState[k] = 1;
  }
  unlockNet();
}

// ================= websocket =================
typedef void (*WsCb)(WStype_t, uint8_t *, size_t);
WebSocketsClient wsCl[MAX_HOSTS];
bool wsOn[MAX_HOSTS] = {false, false, false, false};
char wsId[MAX_HOSTS][40] = {"esp32panel", "esp32panel", "esp32panel", "esp32panel"};
uint32_t lastRebind[MAX_HOSTS] = {0, 0, 0, 0};

float fv(JsonVariant v) { return v.is<float>() || v.is<int>() ? v.as<float>() : -1.0f; }

void wsEventFor(int slot, WStype_t type, uint8_t *pl, size_t len) {
  int idx = slot;
  if (idx < 0 || idx >= nHosts) return;
  NetData &nd = net[idx];
  if (type == WStype_DISCONNECTED) {
    lockNet(); nd.run.connected = false; addLogLocked(nd, L("ws closed", "WS 已断开")); bump(nd); unlockNet();
    return;
  }
  if (type == WStype_CONNECTED) {
    lockNet(); nd.run.connected = true; addLogLocked(nd, L("ws connected", "WS 已连接")); bump(nd); unlockNet();
    return;
  }
  if (type == WStype_BIN) return;
  if (type != WStype_TEXT) return;
  JsonDocument doc;
  if (deserializeJson(doc, pl, len)) return;
  const char *t = doc["type"] | "";
  JsonObject d = doc["data"];
  lockNet();
  if (!strcmp(t, "status")) {
    int qr = d["status"]["exec_info"]["queue_remaining"] | -1;
    if (qr >= 0) nd.run.pending = qr;
  } else if (!strcmp(t, "execution_start")) {
    nd.run.running = true;
    nd.run.start_ms = millis();
    nd.run.pv = nd.run.pmax = 0;
    nd.run.stepSec = 0;
    nd.run.node[0] = 0;
    nd.lastStepVal = -1;
    nd.lastStepMs = millis();
    nd.lastHeartbeat = millis();
    nd.nodesDone = 0;
    nd.nodesCached = 0;
    nd.nodeSeenCount = 0;
    addLogLocked(nd, L("start job", "开始任务"));
  } else if (!strcmp(t, "executing")) {
    if (d["node"].isNull()) {
      nd.run.running = false;
      nd.run.pv = nd.run.pmax = 0;
      nd.run.node[0] = 0;
    } else {
      const char *ndn = d["node"];
      if (ndn) {
        char node[16];
        strlcpy(node, ndn, sizeof(node));
        if (strcmp(node, nd.run.node)) {
          char msg[40];
          snprintf(msg, sizeof(msg), L("node %s", "节点 %s"), node);
          addLogLocked(nd, msg);
        }
        strlcpy(nd.run.node, node, sizeof(nd.run.node));
        uint32_t h = 2166136261u;
        for (const char *cp = node; *cp; cp++) { h ^= (uint8_t)*cp; h *= 16777619u; }
        bool seen = false;
        for (int si = 0; si < nd.nodeSeenCount; si++) if (nd.nodeSeen[si] == h) { seen = true; break; }
        if (!seen && nd.nodeSeenCount < 64) { nd.nodeSeen[nd.nodeSeenCount++] = h; nd.nodesDone++; }
      }
    }
  } else if (!strcmp(t, "progress")) {
    float v = fv(d["value"]);
    float mx = fv(d["max"]);
    if (mx > 0) {
      if (nd.run.pmax != mx) { nd.run.pmax = mx; nd.lastStepVal = -1; }
      if (v > nd.lastStepVal) {
        float dt = nd.lastStepMs ? (millis() - nd.lastStepMs) / 1000.0f : 0;
        nd.run.stepSec = dt;
        nd.lastStepVal = v;
        nd.lastStepMs = millis();
        if (millis() - nd.lastStepLog > 350) {
          nd.lastStepLog = millis();
          char msg[40];
          snprintf(msg, sizeof(msg), L("step %d/%d %.1fs", "步 %d/%d %.1fs"), (int)v, (int)mx, dt);
          addLogLocked(nd, msg);
        }
      }
      nd.run.pv = v;
    }
    const char *ndn = d["node"];
    if (ndn) strlcpy(nd.run.node, ndn, sizeof(nd.run.node));
    if (!nd.run.running) { nd.run.running = true; nd.run.start_ms = millis(); }
  } else if (!strcmp(t, "progress_state")) {
    JsonObject nodes = d["nodes"];
    int done = 0, cached = 0, total = nodes.size();
    const char *rnode = nullptr;
    float rv = 0, rm = 0;
    for (JsonPair kv : nodes) {
      JsonObject n = kv.value();
      const char *st = n["state"] | "";
      if (!strcmp(st, "finished")) done++;
      else if (!strcmp(st, "cached")) cached++;
      else if (!strcmp(st, "running")) {
        rnode = kv.key().c_str();
        rv = fv(n["value"]);
        rm = fv(n["max"]);
      }
    }
    if (total > 0) nd.nodeTotal = total;
    nd.nodesDone = done;
    nd.nodesCached = cached;
    if (rnode && rm > 0) {
      strlcpy(nd.run.node, rnode, sizeof(nd.run.node));
      if (rm != nd.run.pmax) { nd.run.pmax = rm; nd.lastStepVal = -1; }
      if (rv > nd.lastStepVal) {
        float dt = nd.lastStepMs ? (millis() - nd.lastStepMs) / 1000.0f : 0;
        nd.run.stepSec = dt;
        nd.lastStepVal = rv;
        nd.lastStepMs = millis();
        if (millis() - nd.lastStepLog > 350) {
          nd.lastStepLog = millis();
          char msg[40];
          snprintf(msg, sizeof(msg), L("step %d/%d %.1fs", "步 %d/%d %.1fs"), (int)rv, (int)rm, dt);
          addLogLocked(nd, msg);
        }
      }
      nd.run.pv = rv;
      if (!nd.run.running) { nd.run.running = true; nd.run.start_ms = millis(); }
    }
  } else if (!strcmp(t, "execution_cached")) {
    JsonArray arr = d["nodes"];
    if (!arr.isNull()) {
      nd.nodesCached = arr.size();
      char msg[40];
      snprintf(msg, sizeof(msg), L("cached %d nodes", "缓存 %d 个节点"), nd.nodesCached);
      addLogLocked(nd, msg);
    }
  } else if (!strcmp(t, "execution_success")) {
    nd.run.running = false;
    nd.run.pv = nd.run.pmax = 0;
    nd.run.node[0] = 0;
    char msg[40];
    snprintf(msg, sizeof(msg), L("done %.1fs", "完成 %.1fs"), nd.run.start_ms ? (millis() - nd.run.start_ms) / 1000.0f : 0.0f);
    addLogLocked(nd, msg);
    nd.tH = 0;
  } else if (!strcmp(t, "execution_error")) {
    nd.run.running = false;
    nd.run.pv = nd.run.pmax = 0;
    nd.run.node[0] = 0;
    addLogLocked(nd, L("execution error", "执行错误"));
    nd.tH = 0;
  } else if (!strcmp(t, "execution_interrupted")) {
    nd.run.running = false;
    addLogLocked(nd, L("interrupted", "已中断"));
  }
  bump(nd);
  unlockNet();
}
void wsEvent0(WStype_t t, uint8_t *p, size_t l) { wsEventFor(0, t, p, l); }
void wsEvent1(WStype_t t, uint8_t *p, size_t l) { wsEventFor(1, t, p, l); }
void wsEvent2(WStype_t t, uint8_t *p, size_t l) { wsEventFor(2, t, p, l); }
void wsEvent3(WStype_t t, uint8_t *p, size_t l) { wsEventFor(3, t, p, l); }
static const WsCb wsEv[MAX_HOSTS] = {wsEvent0, wsEvent1, wsEvent2, wsEvent3};

// slot == host: every configured server gets its own websocket so the
// client_id follow-up works for all of them at once.
void wsBindSlot(int slot, int h) {
  if (slot < 0 || slot >= MAX_HOSTS) return;
  WebSocketsClient &c = wsCl[slot];
  bool &on = wsOn[slot];
  if (on) { c.disconnect(); on = false; }
  if (h < 0 || h >= nHosts || !net[h].host[0]) return;
  const char *idp = net[h].promptClient[0] ? net[h].promptClient : "esp32panel";
  strlcpy(wsId[slot], idp, 40);
  char path[80];
  snprintf(path, sizeof(path), "/ws?clientId=%s", idp);
  c.begin(net[h].host, net[h].port, path, "arduino");
  c.onEvent(wsEv[slot]);
  c.setReconnectInterval(3000);
  on = true;
}
void wsStopAll() {
  for (int i = 0; i < MAX_HOSTS; i++) {
    if (wsOn[i]) { wsCl[i].disconnect(); wsOn[i] = false; }
  }
}

static int shownJobs() { return useGrid() ? cellJobs() : MAX_JOBS; }

void netTask(void *) {
  int bound[MAX_HOSTS] = {-1, -1, -1, -1};
  uint32_t epoch = 0xFFFFFFFFu;
  for (;;) {
    uint32_t ms = millis();
    if (wst == W_ONLINE && nHosts > 0) {
      if (epoch != netEpoch) {
        epoch = netEpoch;
        for (int h = 0; h < MAX_HOSTS; h++) {
          bound[h] = -1;
          net[h].tV = net[h].tQ = net[h].tH = net[h].tU = 0;
        }
      }
      for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) {
        if (bound[h] != h) { bound[h] = h; wsBindSlot(h, h); }
      }
      for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) {
        NetData &nd = net[h];
        if (nd.run.running && ms - nd.lastHeartbeat > 15000) {
          nd.lastHeartbeat = ms;
          uint32_t secs = nd.run.start_ms ? (ms - nd.run.start_ms) / 1000 : 0;
          logEvent(nd, L("running %02u:%02u", "运行中 %02u:%02u"), (unsigned)(secs / 60), (unsigned)(secs % 60));
        }
        if (!nd.tV || ms - nd.tV > pollVitalsMs()) { nd.tV = ms; fetchVitals(nd); }
        if (!nd.tQ || ms - nd.tQ > pollQueueMs()) { nd.tQ = ms; fetchQueue(nd); }
        if (ultraEnabled && (!nd.tU || ms - nd.tU > 2500)) { nd.tU = ms; fetchUltra(nd); }
        if (!nd.tH || ms - nd.tH > 20000) { nd.tH = ms; fetchHistory(nd); }
        if (nd.run.running && nd.promptClient[0] && strcmp(nd.promptClient, wsId[h]) && ms - lastRebind[h] > 1500) {
          lastRebind[h] = ms;
          logEvent(nd, L("follow %s", "跟随 %s"), nd.promptClient);
          wsBindSlot(h, h);
        }
        int maxK = shownJobs();
        if (maxK > nd.nJobs) maxK = nd.nJobs;
        if (nd.serverOk && nd.wantThumbs && ms - nd.lastThumbTry > 1200) {
          nd.lastThumbTry = ms;
          for (int n = 0; n < MAX_JOBS && n < nd.nJobs; n++) {
            int k = nd.thumbScan % MAX_JOBS;
            nd.thumbScan++;
            if (k >= nd.nJobs) continue;
            if (k >= shownJobs()) { nd.thumbState[k] = 3; continue; }
            if (!thumbSource(nd.jobs[k])) { nd.thumbState[k] = 3; continue; }
            if (nd.thumbState[k] == 1) continue;
            if (nd.thumbState[k] == 2 && (int32_t)(ms - nd.thumbAt[k]) < 0) continue;
            fetchJobThumb(nd, k);
            break;
          }
        }
        if (nd.serverOk && nd.wantThumbs && ms - nd.lastSizeTry > 1500) {
          for (int k = 0; k < maxK; k++) {
            Job &j = nd.jobs[k];
            if (j.sizeKnown || j.videoSec <= 0.05f || !isVideoFile(j.file)) continue;
            if ((int32_t)(ms - j.sizeAt) < 0) continue;
            String vpath = "/view?filename=" + urlEnc(j.file) + "&type=" + urlEnc(j.type) +
                           "&subfolder=" + urlEnc(j.subfolder);
            uint32_t sz = 0;
            if (httpHeadSize(nd, vpath, sz)) {
              lockNet(); j.sizeBytes = sz; j.sizeKnown = true; bump(nd); unlockNet();
            } else {
              j.sizeAt = ms + 120000;
            }
            nd.lastSizeTry = ms;
            break;
          }
        }
      }
      for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) if (wsOn[h]) wsCl[h].loop();
    } else {
      wsStopAll();
      for (int h = 0; h < MAX_HOSTS; h++) bound[h] = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}


// ================= WiFi =================
void startConnect() {
  if (cfgSsid.length() == 0) { wst = W_NONE; return; }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(cfgSsid.c_str(), cfgPsk.c_str());
  wifiTrySince = millis();
  wst = W_CONNECTING;
}
void handleWifi() {
  if (wst == W_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      wst = W_ONLINE;
      Serial.printf("WiFi online %s\n", WiFi.localIP().toString().c_str());
      configTime(8 * 3600, 0, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
      if (!shotSrvStarted) { shotSrv.begin(); shotSrvStarted = true; }
      updateWifiStatus();
      netConfigure();
      if (nHosts > 0) {
        buildDashboard();
        lv_scr_load(scrDash);
        netEpoch++;
      } else showSetupTab(1);
    } else if (millis() - wifiTrySince > 25000) {
      wst = W_NONE;
      setupMsg = L("connect failed, check password", "连接失败，请检查密码");
      updateWifiStatus();
    }
  } else if (wst == W_ONLINE) {
    if (WiFi.status() != WL_CONNECTED) {
      wst = W_CONNECTING;
      wifiTrySince = millis();
      setupMsg = L("wifi lost, reconnecting...", "WiFi 断开，重连中…");
      updateWifiStatus();
      WiFi.begin(cfgSsid.c_str(), cfgPsk.c_str());
    }
  }
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println("\n=== ComfyUI panel boot (v4) ===");
  Serial.printf("heap=%u psram=%u\n", ESP.getFreeHeap(), ESP.getPsramSize());

  netMux = xSemaphoreCreateMutex();
  loadCfg();
  TH = THEMES[themeIdx];

  Wire.begin(I2C_SDA, I2C_SCL, 400000);
  TCA.begin();
  TCA.pinMode1(1, OUTPUT);
  lcd_reset();

  if (!touch.begin(Wire, FT6X36_SLAVE_ADDRESS)) Serial.println("FT6336 not found");
  if (!gfx->begin(LCD_SPI_HZ)) Serial.println("ST7796 begin FAILED");
  gfx->setRotation(GFX_ROT);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  lv_init();
  uint32_t sw = gfx->width(), sh = gfx->height();
  bufSize = sw * 40;
  disp_buf1 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  disp_buf2 = (lv_color_t *)heap_caps_malloc(bufSize * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  static lv_disp_draw_buf_t db;
  lv_disp_draw_buf_init(&db, disp_buf1, disp_buf2, bufSize);
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = sw;
  dd.ver_res = sh;
  dd.flush_cb = my_disp_flush;
  dd.draw_buf = &db;
  lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER;
  id.read_cb = my_touch_read;
  lv_indev_drv_register(&id);

  fbMirror = (uint16_t *)heap_caps_malloc(UI_W * UI_H * 2, MALLOC_CAP_SPIRAM);
  if (fbMirror) memset(fbMirror, 0, UI_W * UI_H * 2);
  scrThumb = (uint16_t *)heap_caps_malloc(THUMB_W * THUMB_H * 2, MALLOC_CAP_SPIRAM);
  thumbJpg = (uint8_t *)heap_caps_malloc(THUMB_JPEG_MAX, MALLOC_CAP_SPIRAM);
  netConfigure();

  shotSrv.on("/shot.raw", HTTP_GET, handleShot);
  shotSrv.on("/debug/show", HTTP_GET, []() {
    String s = shotSrv.arg("s");
    if (editOverlay) lv_obj_add_flag(editOverlay, LV_OBJ_FLAG_HIDDEN);
    if (s == "setup") { showSetupTab(0); updateWifiStatus(); }
    else if (s == "servers") { showSetupTab(1); updateWifiStatus(); }
    else if (s == "theme") { showSetupTab(2); updateWifiStatus(); }
    else if (scrDash) lv_scr_load(scrDash);
    shotSrv.send(200, "text/plain", "ok");
  });
  shotSrv.on("/debug/scan", HTTP_GET, []() { doScan(); shotSrv.send(200, "text/plain", "ok"); });
  shotSrv.on("/debug/edit", HTTP_GET, []() {
    showSetupTab(1);
    int i = shotSrv.arg("i").toInt();
    if (i >= 0 && i < MAX_HOSTS) {
      char cur[64] = "";
      if (hostStr[i].length()) snprintf(cur, sizeof(cur), "%s:%d", hostStr[i].c_str(), hostPort[i]);
      openEditor(i, L("Server ip[:port]", "服务器 ip[:端口]"), cur, false);
    }
    shotSrv.send(200, "text/plain", "ok");
  });
  shotSrv.on("/debug/page", HTTP_GET, []() {
    int i = shotSrv.arg("i").toInt();
    if (i >= 0 && i < pageCount() && i != activePageIdx) {
      switchPage(i);
      if (tileView) lv_obj_set_tile_id(tileView, i, 0, LV_ANIM_OFF);
    }
    shotSrv.send(200, "text/plain", "ok");
  });
  shotSrv.on("/debug/theme", HTTP_GET, []() {
    int i = shotSrv.arg("i").toInt();
    if (i >= 0 && i < 10) { prefs.begin("cfg", false); prefs.putInt("theme", i); prefs.end(); }
    shotSrv.send(200, "text/plain", "rebooting");
    delay(200); ESP.restart();
  });
  shotSrv.on("/debug/lang", HTTP_GET, []() {
    int i = shotSrv.arg("i").toInt();
    if (i >= 0 && i <= 1) { prefs.begin("cfg", false); prefs.putInt("lang", i); prefs.end(); }
    shotSrv.send(200, "text/plain", "rebooting");
    delay(200); ESP.restart();
  });
  shotSrv.on("/debug/ultra", HTTP_GET, []() {
    if (shotSrv.hasArg("i")) ultraEnabled = shotSrv.arg("i").toInt() != 0;
    else ultraEnabled = !ultraEnabled;
    if (shotSrv.hasArg("p")) { int p = shotSrv.arg("p").toInt(); if (p > 0 && p <= 65535) ultraPort = p; }
    saveCfg();
    if (nHosts > 0 && wst == W_ONLINE) { buildDashboard(); lv_scr_load(scrDash); netEpoch++; }
    shotSrv.send(200, "text/plain", ultraEnabled ? "ultra on" : "ultra off");
  });
  shotSrv.on("/debug/delhost", HTTP_GET, []() {
    int i = shotSrv.arg("i").toInt();
    if (i >= 0 && i < nHosts && i < MAX_HOSTS) {
      for (int k = i; k < MAX_HOSTS - 1; k++) { hostStr[k] = hostStr[k + 1]; hostPort[k] = hostPort[k + 1]; }
      hostStr[MAX_HOSTS - 1] = "";
      hostPort[MAX_HOSTS - 1] = DEFAULT_PORT;
      nHosts--;
      applyHostsAndShow();
      shotSrv.send(200, "text/plain", "ok");
    } else shotSrv.send(400, "text/plain", "bad");
  });
  shotSrv.on("/debug/thumb", HTTP_GET, []() {
    lockNet();
    for (int k = 0; k < MAX_JOBS; k++) {
      net[activeHost].thumbState[k] = 0;
      net[activeHost].thumbAt[k] = 0;
      net[activeHost].thumbSig[k][0] = 0;
    }
    net[activeHost].wantThumbs = true;
    unlockNet();
    shotSrv.send(200, "text/plain", "ok");
  });
  shotSrv.on("/debug/host", HTTP_GET, []() {
    int i = shotSrv.arg("i").toInt();
    String h = shotSrv.arg("h");
    if (i >= 0 && i < MAX_HOSTS && h.length()) {
      parseHost(i, h);
      if (i >= nHosts) nHosts = i + 1;
      applyHostsAndShow();
      shotSrv.send(200, "text/plain", "ok");
    } else shotSrv.send(400, "text/plain", "bad");
  });
  shotSrv.on("/debug/addhost", HTTP_GET, []() {
    String h = shotSrv.arg("h");
    if (h.length() && nHosts < MAX_HOSTS) {
      parseHost(nHosts, h);
      nHosts++;
      applyHostsAndShow();
    }
    shotSrv.send(200, "text/plain", "ok");
  });
  shotSrv.on("/stats", HTTP_GET, []() {
    int on = 0, ws = 0, rn = 0, q = 0, seen = 1;
    for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) {
      if (dview[h].serverOk) on++;
      if (net[h].run.connected) ws = 1;
      if (dview[h].run.running) rn = 1;
      q += dview[h].run.pending;
      if (!dview[h].everSeen) seen = 0;
    }
    int srv;
    if (useGrid()) srv = (on == nHosts) ? 1 : 0;
    else { srv = (int)serverOk; ws = (int)run.connected; rn = (int)run.running; q = run.pending; seen = (int)everSeen; }
    char b[224];
    snprintf(b, sizeof(b), "{\"wifi\":%d,\"server\":%d,\"ws\":%d,\"running\":%d,\"pending\":%d,\"hosts\":%d,\"on\":%d,\"cols\":%d,\"rows\":%d,\"page\":%d,\"seen\":%d,\"up\":%lu,\"rst\":%d,\"heap\":%u,\"ui_max_ms\":%u}",
             (int)wst, srv, ws, rn, q, nHosts, on, gridCols(), gridRows(), activePageIdx, seen,
             (unsigned long)(millis() / 1000), (int)esp_reset_reason(),
             ESP.getFreeHeap(), (unsigned)(uiMaxUs / 1000));
    shotSrv.send(200, "application/json", b);
  });

  buildSetup();
  if (nHosts > 0) buildDashboard();
  startConnect();
  updateWifiStatus();

  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
  Serial.println("setup done");
}

uint32_t tPull = 0, tClock = 0;

void handleBootButton() {
  static uint32_t downSince = 0;
  static bool fired = false;
  if (digitalRead(0) == LOW) {
    if (!downSince) downSince = millis();
    if (!fired && millis() - downSince > 3000) {
      fired = true;
      Serial.println("BOOT held: clearing config");
      prefs.begin("cfg", false);
      prefs.clear();
      prefs.end();
      delay(150);
      ESP.restart();
    }
  } else downSince = 0;
}

void loop() {
  uint32_t us0 = micros();
  lv_timer_handler();
  uint32_t uiDur = micros() - us0;
  if (uiDur > uiMaxUs) uiMaxUs = uiDur;
  handleWifi();
  handleBootButton();
  uint32_t ms = millis();
  if (shotSrvStarted) shotSrv.handleClient();

  if (scrDash && lv_scr_act() == scrDash && ms - tPull > 150) {
    tPull = ms;
    if (useGrid()) {
      for (int h = 0; h < nHosts && h < MAX_HOSTS; h++) pullHost(h);
      if (activePageIdx == 0) { if (ultraEnabled) refreshUltraDash(); else refreshGridDash(); }
      else if (activePageIdx == 1) refreshGridLogs();
      else refreshGridOut();
      refreshHeader();
    } else {
      bool changed = pullActive();
      if (changed) refreshHeader();
      if (activePage == 0) refreshMonitor();
      else refreshOutputs();
    }
    refreshClock();
  }
  vTaskDelay(pdMS_TO_TICKS(4));
}
