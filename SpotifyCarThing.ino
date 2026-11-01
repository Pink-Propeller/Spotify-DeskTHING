#include <WiFi.h>
#include <esp_wifi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <base64.h>
#include <time.h>
#include <functional>
#include "InterFonts.h"


// ===================== YOUR SETTINGS =====================
#define SPOTIFY_CLIENT_ID     "your-spotify-client-id"
#define SPOTIFY_CLIENT_SECRET "your-spotify-client-secret"
// =========================================================

#define REDIRECT_URI    "http://127.0.0.1:8888/callback"
#define DEVICE_HOSTNAME "carthing"
#define TZ_INFO         "NZST-12NZDT,M9.5.0,M4.1.0/3"

#define ENC_A 14
#define ENC_B 21
#define ENC_STEPS_PER_DETENT 4
#define ENC_REVERSE false

#define SCREEN_ROTATION 1
#define TOUCH_ROTATION  2

#define EDGE_PX 26
#define SPI_WRITE_MHZ 80

char wifiSsid[33] = "";
char wifiPass[65] = "";
char joinSsid[33] = "";
char joinPass[65] = "";
bool joinOpen = false;
bool joining = false;
uint32_t joinStart = 0, bootStart = 0;

#define MAX_PL 150
#define MAX_TR 500
#define THUMB  64
#define ART    92
#define NTHUMB (MAX_PL + 6)

#define RGB565(r, g, b) ((uint16_t)(((((uint8_t)(r)) & 0xF8) << 8) | ((((uint8_t)(g)) & 0xFC) << 3) | (((uint8_t)(b)) >> 3)))

struct Player {
  bool active = false, playing = false, shuffle = false, supportsVolume = true, liked = false, isTrack = true;
  uint8_t repeat = 0;
  int volume = -1;
  uint32_t progress = 0, duration = 0, fetchedAt = 0;
  char trackId[40] = "", uri[64] = "", title[96] = "", artist[96] = "", album[96] = "", art[200] = "", device[48] = "", context[64] = "";
};

struct PL {
  char id[28];
  char name[64];
  char sub[48];
  char img[200];
  uint16_t total;
  bool own;
};

struct TR {
  char uri[48];
  char name[72];
  char artist[64];
  uint32_t dur;
};

enum CmdType : uint8_t { C_TOGGLE, C_NEXT, C_PREV, C_SHUFFLE, C_REPEAT, C_SEEK, C_LIKE, C_OPEN_LIST, C_MORE, C_PLAY_AT, C_RELOAD_PL };
struct Cmd { uint8_t type; int32_t a; int32_t b; char s[64]; };

enum Screen : uint8_t { SCR_BOOT, SCR_SETUP, SCR_IDLE, SCR_LIBRARY, SCR_LIST, SCR_NOW, SCR_WIFI, SCR_WIFIPASS };
struct WNet { char ssid[33]; int8_t rssi; bool open; };
enum ListKind : uint8_t { LK_LIKED, LK_PLAYLIST, LK_ALBUM };
enum RecentState : uint8_t { RS_IDLE, RS_LOADING, RS_OK, RS_SCOPE, RS_ERROR };
struct RecentItem { char key[40]; char name[64]; char owner[48]; char img[200]; uint8_t kind; int16_t plIdx; uint16_t total; };
struct Key { int16_t x, y, w, h; char c; uint8_t kind; };
enum Overlay : uint8_t { OV_NONE, OV_SETTINGS, OV_ACCOUNT };

struct Scroller {
  float y = 0, vel = 0, maxY = 0;
};
struct TouchState {
  bool down = false, moved = false, swallow = false;
  int x = 0, y = 0, sx = 0, sy = 0, py = 0;
  uint32_t t0 = 0, lastSeen = 0, lastMove = 0;
  float vel = 0;
  int target = 0;
};

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel;
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
  lgfx::Touch_FT5x06 _touch;
public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host = SPI2_HOST;
      c.spi_mode = 0;
      c.freq_write = SPI_WRITE_MHZ * 1000000;
      c.freq_read = 16000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 12;
      c.pin_mosi = 11;
      c.pin_miso = 13;
      c.pin_dc = 46;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = 10;
      c.pin_rst = -1;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.offset_x = 0;
      c.offset_y = 0;
      c.offset_rotation = 0;
      c.readable = true;
      c.invert = true;
      c.rgb_order = false;
      c.dlen_16bit = false;
      c.bus_shared = false;
      _panel.config(c);
    }
    {
      auto c = _light.config();
      c.pin_bl = 45;
      c.invert = false;
      c.freq = 44100;
      c.pwm_channel = 7;
      _light.config(c);
      _panel.setLight(&_light);
    }
    {
      auto c = _touch.config();
      c.x_min = 0;
      c.x_max = 239;
      c.y_min = 0;
      c.y_max = 319;
      c.pin_int = 17;
      c.bus_shared = false;
      c.offset_rotation = TOUCH_ROTATION;
      c.i2c_port = 0;
      c.i2c_addr = 0x38;
      c.pin_sda = 16;
      c.pin_scl = 15;
      c.freq = 400000;
      _touch.config(c);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

LGFX lcd;
LGFX_Sprite spr(&lcd);
LGFX_Sprite prevSpr(&lcd);
LGFX_Sprite artSpr(&lcd);


uint16_t C_BG     = RGB565(0, 0, 0);
uint16_t C_CARD   = RGB565(0x1F, 0x1F, 0x1F);
uint16_t C_CARD2  = RGB565(0x2A, 0x2A, 0x2A);
uint16_t C_MINI   = RGB565(0x1A, 0x1A, 0x1A);
uint16_t C_TRACK  = RGB565(0x3E, 0x3E, 0x3E);
uint16_t C_TEXT   = RGB565(255, 255, 255);
uint16_t C_SUB    = RGB565(0xA7, 0xA7, 0xA7);
uint16_t C_SUB2   = RGB565(0xB3, 0xB3, 0xB3);
uint16_t C_DIM    = RGB565(105, 105, 105);
uint16_t C_ON     = RGB565(0, 0, 0);
float TINT_K      = 1.1f;
const uint16_t C_GREEN  = RGB565(0x1D, 0xB9, 0x54);
const uint16_t C_ORANGE = RGB565(255, 150, 0);
const uint16_t C_RED    = RGB565(235, 52, 52);
const uint16_t C_BLACK  = RGB565(0, 0, 0);
const uint16_t C_YELLOW = RGB565(255, 204, 51);
const uint16_t C_DKGREEN = RGB565(20, 140, 62);

bool themeLight = false;
uint8_t sleepIdx = 2;
bool saverOn = false;
uint8_t saverKind = 0;
bool clock24 = false;
uint8_t brightness = 255;
const uint32_t SLEEP_MS[] = {60000UL, 300000UL, 600000UL, 1200000UL, 0UL};
const char* SLEEP_LABEL[] = {"1 min", "5 min", "10 min", "20 min", "Never"};

void applyTheme(bool light) {
  themeLight = light;
  if (light) {
    C_BG = RGB565(244, 244, 244); C_CARD = RGB565(228, 228, 228); C_CARD2 = RGB565(214, 214, 214);
    C_MINI = RGB565(232, 232, 232); C_TRACK = RGB565(200, 200, 200);
    C_TEXT = RGB565(20, 20, 20); C_SUB = RGB565(95, 95, 95); C_SUB2 = RGB565(80, 80, 80); C_DIM = RGB565(150, 150, 150);
    C_ON = RGB565(250, 250, 250); TINT_K = 0.35f;
  } else {
    C_BG = RGB565(0, 0, 0); C_CARD = RGB565(0x1F, 0x1F, 0x1F); C_CARD2 = RGB565(0x2A, 0x2A, 0x2A);
    C_MINI = RGB565(0x1A, 0x1A, 0x1A); C_TRACK = RGB565(0x3E, 0x3E, 0x3E);
    C_TEXT = RGB565(255, 255, 255); C_SUB = RGB565(0xA7, 0xA7, 0xA7); C_SUB2 = RGB565(0xB3, 0xB3, 0xB3); C_DIM = RGB565(105, 105, 105);
    C_ON = RGB565(0, 0, 0); TINT_K = 1.1f;
  }
}

lgfx::PointerWrapper fontWrap[13];
lgfx::VLWfont fontVlw[13];
#define F_R9   (&fontVlw[0])
#define F_R10  (&fontVlw[1])
#define F_R11  (&fontVlw[2])
#define F_R12  (&fontVlw[3])
#define F_SB8  (&fontVlw[4])
#define F_SB9  (&fontVlw[5])
#define F_SB11 (&fontVlw[6])
#define F_SB12 (&fontVlw[7])
#define F_B15  (&fontVlw[8])
#define F_B16  (&fontVlw[9])
#define F_B22  (&fontVlw[10])
#define F_B30  (&fontVlw[11])
#define F_L92  (&fontVlw[12])
#define F_TINY  F_R11
#define F_SMALL F_R12
#define F_SMB   F_SB12
#define F_MED   F_B16
#define F_BIG   F_B22
#define F_HUGE  F_B30

void loadFonts() {
  const uint8_t* d[13] = {INTER_R9, INTER_R10, INTER_R11, INTER_R12, INTER_SB8, INTER_SB9, INTER_SB11, INTER_SB12, INTER_B15, INTER_B16, INTER_B22, INTER_B30, INTER_L92};
  const uint32_t l[13] = {INTER_R9_LEN, INTER_R10_LEN, INTER_R11_LEN, INTER_R12_LEN, INTER_SB8_LEN, INTER_SB9_LEN, INTER_SB11_LEN, INTER_SB12_LEN, INTER_B15_LEN, INTER_B16_LEN, INTER_B22_LEN, INTER_B30_LEN, INTER_L92_LEN};
  for (int i = 0; i < 13; i++) {
    fontWrap[i].set(d[i], l[i]);
    if (!fontVlw[i].loadFont(&fontWrap[i])) Serial.printf("[font] %d failed to load\n", i);
  }
}

struct SpiRamAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
} spiAlloc;

SemaphoreHandle_t mux;
#define LOCK()   xSemaphoreTakeRecursive(mux, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(mux)

QueueHandle_t cmdQ;
Preferences prefs;
WebServer server(80);
WiFiClientSecure apiClient, imgClient;
HTTPClient apiHttp;

Player shared;
uint32_t holdPlayUntil = 0, holdShuffleUntil = 0, holdRepeatUntil = 0, holdVolUntil = 0, holdLikeUntil = 0, holdSeekUntil = 0;

char refreshToken[256] = "";
char accessToken[512] = "";
uint32_t tokenExpiry = 0;
char myId[64] = "", myName[64] = "";
char likePl[28] = "", likePlName[64] = "";
char authState[17] = "";
char myAvatar[200] = "";
volatile bool needAvatar = false;
uint8_t* avatarBuf = nullptr;
volatile int avatarBufLen = 0;
volatile bool avatarPending = false;

volatile bool needMe = false, needPlaylists = false, needLikeCheck = false;
volatile bool timeStarted = false;
volatile bool screenAsleep = false;
volatile uint8_t uiScreen = 0;

PL* pls = nullptr;
volatile int plCount = 0;
volatile bool plLoading = false;
volatile bool plLoadedOnce = false;
LGFX_Sprite* thumbs[NTHUMB];
volatile uint8_t thumbState[NTHUMB];
RecentItem recent[6];
volatile int recentCount = 0;
volatile uint8_t recentState = RS_IDLE;
volatile bool needRecent = false;
uint32_t recentAt = 0;
uint8_t* thumbBuf = nullptr;
volatile int thumbBufLen = 0;
volatile int thumbBufIdx = -1;

TR* trs = nullptr;
volatile int trCount = 0, trTotal = 0;
char trList[40] = "";
volatile bool trLoading = false;
volatile uint16_t trGen = 0;

uint8_t* artBuf = nullptr;
volatile int artBufLen = 0;
volatile bool artPending = false;
char artFetchedUrl[200] = "";
char artShownUrl[200] = "";
uint16_t artTint = C_BG;

volatile int volTarget = -1;
volatile bool volDirty = false;
volatile uint32_t volChangedAt = 0;
uint32_t lastVolSent = 0;
volatile uint32_t rateLimitedUntil = 0;

char toastMsg[96] = "";
uint32_t toastAt = 0;

volatile int32_t encRaw = 0;

volatile int lastPlayerCode = 0;
volatile uint32_t lastGoodPoll = 0;
uint8_t emptyPolls = 0, failPolls = 0;
volatile bool pollNow = false;
char wantUri[64] = "", skipFromUri[64] = "";
uint32_t holdTrackUntil = 0;
volatile int trError = 0;
volatile uint32_t lastRateToast = 0;

// ---------------------------------------------------------------- text helpers

const char* foldCp(uint32_t cp) {
  static const char latin[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  static char one[2];
  if (cp >= 0xC0 && cp <= 0xFF) { one[0] = latin[cp - 0xC0]; one[1] = 0; return one; }
  switch (cp) {
    case 0x2018: case 0x2019: case 0x00B4: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2013: case 0x2014: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x00B7: case 0x2022: return "-";
    case 0x00A0: return " ";
    case 0x0152: return "OE";
    case 0x0153: return "oe";
    case 0x0160: return "S";
    case 0x0161: return "s";
    case 0x017D: return "Z";
    case 0x017E: return "z";
    case 0x0141: return "L";
    case 0x0142: return "l";
  }
  if (cp >= 0x100 && cp < 0x180) {
    static const char ext[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    uint32_t i = cp - 0x100;
    if (i < sizeof(ext) - 1) { one[0] = ext[i]; one[1] = 0; return one; }
  }
  return "";
}

void copyFold(char* dst, size_t cap, const char* src) {
  size_t o = 0;
  const uint8_t* p = (const uint8_t*)(src ? src : "");
  while (*p && o + 1 < cap) {
    uint8_t c = *p;
    if (c < 0x80) { dst[o++] = (char)c; p++; continue; }
    uint32_t cp; int n;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 3; }
    else { p++; continue; }
    p++;
    for (int i = 0; i < n && *p; i++, p++) cp = (cp << 6) | (*p & 0x3F);
    const char* r = foldCp(cp);
    while (*r && o + 1 < cap) dst[o++] = *r++;
  }
  dst[o] = 0;
  while (o > 0 && dst[o - 1] == ' ') dst[--o] = 0;
  size_t s = 0;
  while (dst[s] == ' ') s++;
  if (s) memmove(dst, dst + s, o - s + 1);
}

void copyStr(char* dst, size_t cap, const char* src) {
  strncpy(dst, src ? src : "", cap - 1);
  dst[cap - 1] = 0;
}

String urlEnc(const char* s) {
  String o;
  const char* hex = "0123456789ABCDEF";
  for (; *s; s++) {
    char c = *s;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { o += '%'; o += hex[(c >> 4) & 15]; o += hex[c & 15]; }
  }
  return o;
}

String urlDec(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '+') o += ' ';
    else if (c == '%' && i + 2 < s.length()) { o += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16); i += 2; }
    else o += c;
  }
  return o;
}

String jsonEsc(const char* s) {
  String o;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') o += '\\';
    o += *s;
  }
  return o;
}

String htmlEsc(const char* s) {
  String o;
  for (; *s; s++) {
    switch (*s) {
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      case '&': o += "&amp;"; break;
      case '"': o += "&quot;"; break;
      default: o += *s;
    }
  }
  return o;
}

void setToast(const char* m) {
  LOCK();
  copyStr(toastMsg, sizeof(toastMsg), m);
  toastAt = millis();
  UNLOCK();
  Serial.printf("[toast] %s\n", m);
}

void sendCmd(uint8_t type, int32_t a = 0, int32_t b = 0, const char* s = "") {
  Cmd c;
  c.type = type;
  c.a = a;
  c.b = b;
  copyStr(c.s, sizeof(c.s), s);
  xQueueSend(cmdQ, &c, 0);
}

// ---------------------------------------------------------------- storage

void loadSettings() {
  prefs.begin("carthing", true);
  copyStr(refreshToken, sizeof(refreshToken), prefs.getString("rt", "").c_str());
  copyStr(myId, sizeof(myId), prefs.getString("uid", "").c_str());
  copyStr(myName, sizeof(myName), prefs.getString("uname", "").c_str());
  copyStr(likePl, sizeof(likePl), prefs.getString("likepl", "").c_str());
  copyStr(likePlName, sizeof(likePlName), prefs.getString("likename", "").c_str());
  copyStr(wifiSsid, sizeof(wifiSsid), prefs.getString("wssid", "").c_str());
  copyStr(wifiPass, sizeof(wifiPass), prefs.getString("wpass", "").c_str());
  themeLight = prefs.getBool("light", false);
  sleepIdx = prefs.getUChar("sleep", 2);
  saverOn = prefs.getBool("saver", false);
  saverKind = prefs.getUChar("saverk3", 0);
  clock24 = prefs.getBool("c24", false);
  brightness = max(20, (int)prefs.getUChar("bright", 255));
  prefs.end();
  if (sleepIdx > 4) sleepIdx = 2;
  if (saverKind > 3) saverKind = 0;
  applyTheme(themeLight);
}

void saveWifi() {
  prefs.begin("carthing", false);
  prefs.putString("wssid", wifiSsid);
  prefs.putString("wpass", wifiPass);
  prefs.end();
}

void saveUiPrefs() {
  LOCK();
  prefs.begin("carthing", false);
  prefs.putBool("light", themeLight);
  prefs.putUChar("sleep", sleepIdx);
  prefs.putBool("saver", saverOn);
  prefs.putUChar("saverk3", saverKind);
  prefs.putUChar("bright", brightness);
  prefs.putBool("c24", clock24);
  prefs.end();
  UNLOCK();
}

void saveStr(const char* key, const char* val) {
  LOCK();
  prefs.begin("carthing", false);
  prefs.putString(key, val);
  prefs.end();
  UNLOCK();
}

bool loggedIn() {
  return refreshToken[0] != 0;
}

void logout() {
  LOCK();
  prefs.begin("carthing", false);
  for (const char* k : {"rt", "uid", "uname", "likepl", "likename"}) prefs.remove(k);
  prefs.end();
  refreshToken[0] = accessToken[0] = myId[0] = myName[0] = likePl[0] = likePlName[0] = 0;
  myAvatar[0] = 0;
  plCount = 0;
  trCount = 0;
  shared = Player();
  UNLOCK();
  Serial.println("[auth] logged out");
}

// ---------------------------------------------------------------- auth

bool tokenRequest(const String& form) {
  HTTPClient h;
  WiFiClientSecure tls;
  tls.setInsecure();
  if (!h.begin(tls, "https://accounts.spotify.com/api/token")) return false;
  h.setTimeout(10000);
  h.addHeader("Content-Type", "application/x-www-form-urlencoded");
  h.addHeader("Authorization", "Basic " + base64::encode(String(SPOTIFY_CLIENT_ID) + ":" + SPOTIFY_CLIENT_SECRET));
  int code = h.POST(form);
  String body = h.getString();
  h.end();
  if (code != 200) {
    Serial.printf("[auth] token request failed %d: %s\n", code, body.c_str());
    return false;
  }
  JsonDocument doc(&spiAlloc);
  if (deserializeJson(doc, body)) return false;
  LOCK();
  copyStr(accessToken, sizeof(accessToken), doc["access_token"] | "");
  tokenExpiry = millis() + (uint32_t)(doc["expires_in"] | 3600) * 1000UL;
  const char* rt = doc["refresh_token"] | "";
  if (rt[0] && strcmp(rt, refreshToken) != 0) {
    copyStr(refreshToken, sizeof(refreshToken), rt);
    saveStr("rt", refreshToken);
  }
  UNLOCK();
  Serial.println("[auth] access token ready");
  return accessToken[0] != 0;
}

bool refreshAccessToken() {
  if (!loggedIn()) return false;
  String form = "grant_type=refresh_token&refresh_token=" + urlEnc(refreshToken);
  bool ok = tokenRequest(form);
  if (!ok) {
    accessToken[0] = 0;
  }
  return ok;
}

bool ensureToken() {
  if (accessToken[0] && (int32_t)(tokenExpiry - millis()) > 60000) return true;
  return refreshAccessToken();
}

String authorizeUrl() {
  for (int i = 0; i < 16; i++) authState[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[esp_random() % 36];
  authState[16] = 0;
  String scope = "user-read-playback-state user-modify-playback-state user-read-currently-playing "
                 "playlist-read-private playlist-read-collaborative playlist-modify-public playlist-modify-private "
                 "user-library-read user-library-modify user-read-recently-played";
  return String("https://accounts.spotify.com/authorize?response_type=code&client_id=") + SPOTIFY_CLIENT_ID +
         "&redirect_uri=" + urlEnc(REDIRECT_URI) + "&scope=" + urlEnc(scope.c_str()) +
         "&state=" + authState + "&show_dialog=true";
}

bool exchangeCode(const String& code) {
  String form = "grant_type=authorization_code&code=" + urlEnc(code.c_str()) + "&redirect_uri=" + urlEnc(REDIRECT_URI);
  if (!tokenRequest(form)) return false;
  if (!refreshToken[0]) return false;
  needMe = true;
  needPlaylists = true;
  return true;
}

// ---------------------------------------------------------------- api

int api(const char* method, const String& path, const String& body = "", String* out = nullptr) {
  if ((int32_t)(rateLimitedUntil - millis()) > 0) return 429;
  if (!ensureToken()) return -1;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!apiHttp.begin(apiClient, "https://api.spotify.com/v1" + path)) return -1;
    apiHttp.setReuse(true);
    apiHttp.setTimeout(10000);
    const char* keys[] = {"Retry-After"};
    apiHttp.collectHeaders(keys, 1);
    apiHttp.addHeader("Authorization", String("Bearer ") + accessToken);
    if (body.length()) apiHttp.addHeader("Content-Type", "application/json");
    else if (strcmp(method, "GET") != 0 && strcmp(method, "DELETE") != 0) apiHttp.addHeader("Content-Length", "0");
    uint32_t t0 = millis();
    int code = apiHttp.sendRequest(method, body);
    String resp;
    bool isGet = strcmp(method, "GET") == 0;
    int size = code > 0 ? apiHttp.getSize() : 0;
    bool noBody = code == 204 || code == 304 || ((code == 202 || !isGet) && size <= 0);
    if (code > 0 && !noBody) resp = apiHttp.getString();
    if (!isGet) Serial.printf("[api] %s %s -> %d (%lums)\n", method, path.c_str(), code, (unsigned long)(millis() - t0));
    String retry = apiHttp.header("Retry-After");
    apiHttp.end();
    if (code < 0) {
      apiClient.stop();
      if (attempt == 0) continue;
      Serial.printf("[api] %s %s -> connection error %d\n", method, path.c_str(), code);
      return code;
    }
    if (code == 401 && attempt == 0) {
      accessToken[0] = 0;
      if (!refreshAccessToken()) return 401;
      continue;
    }
    if (code == 429) {
      int s = retry.toInt();
      Serial.printf("[api] rate limited, Spotify asked for %ds\n", s);
      s = constrain(s, 5, 60);
      rateLimitedUntil = millis() + s * 1000UL;
      if (millis() - lastRateToast > 60000) {
        lastRateToast = millis();
        char m[80];
        snprintf(m, sizeof(m), "Spotify asked to slow down. Retrying in %ds", s);
        setToast(m);
      }
    }
    if (code >= 400) Serial.printf("[api] %s %s -> %d %s\n", method, path.c_str(), code, resp.substring(0, 160).c_str());
    if (out) *out = resp;
    return code;
  }
  return -1;
}

int download(const char* url, uint8_t* buf, int cap) {
  HTTPClient h;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 50000) apiClient.stop();
  imgClient.setInsecure();
  h.useHTTP10(true);
  h.setReuse(false);
  h.setTimeout(8000);
  if (!h.begin(imgClient, url)) return -1;
  int code = h.GET();
  if (code != 200) {
    Serial.printf("[img] download failed %d, internal heap %u\n", code, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    h.end();
    return -1;
  }
  WiFiClient* s = h.getStreamPtr();
  int want = h.getSize();
  int got = 0;
  uint32_t last = millis();
  while (got < cap && (want < 0 || got < want) && millis() - last < 6000) {
    int av = s->available();
    if (av > 0) {
      int n = s->read(buf + got, min(av, cap - got));
      if (n > 0) { got += n; last = millis(); }
    } else if (!s->connected()) {
      break;
    } else {
      vTaskDelay(2);
    }
  }
  h.end();
  return got;
}

String pickImage(JsonArray imgs, int want) {
  const char* best = "";
  int bestDiff = 1 << 30;
  for (JsonObject im : imgs) {
    const char* u = im["url"] | "";
    int w = im["width"] | 0;
    if (!u[0]) continue;
    if (w == 0) w = 640;
    int d = abs(w - want);
    if (d < bestDiff) { bestDiff = d; best = u; }
  }
  return String(best);
}

String findDevice() {
  String body;
  if (api("GET", "/me/player/devices", "", &body) != 200) return "";
  JsonDocument doc(&spiAlloc);
  if (deserializeJson(doc, body)) return "";
  String first;
  for (JsonObject d : doc["devices"].as<JsonArray>()) {
    const char* id = d["id"] | "";
    if (!id[0] || (d["is_restricted"] | false)) continue;
    if (d["is_active"] | false) return String(id);
    if (!first.length()) first = id;
  }
  return first;
}

void dropHolds() {
  LOCK();
  holdPlayUntil = holdShuffleUntil = holdRepeatUntil = holdSeekUntil = holdTrackUntil = 0;
  wantUri[0] = skipFromUri[0] = 0;
  UNLOCK();
  pollNow = true;
}

int playerCall(const char* method, const String& path, const String& body = "") {
  int code = api(method, path, body);
  if (code == 404) {
    String dev = findDevice();
    if (!dev.length()) {
      setToast("No Spotify device found. Open Spotify on your phone or PC.");
      return 404;
    }
    String p = path + (path.indexOf('?') >= 0 ? "&" : "?") + "device_id=" + dev;
    code = api(method, p, body);
  }
  if (code == 403) setToast("Spotify Premium is needed for playback control");
  if (code < 200 || code >= 300) {
    Serial.printf("[cmd] %s %s failed %d, resyncing\n", method, path.c_str(), code);
    dropHolds();
  }
  return code;
}

uint32_t lastMeTry = 0;

void fetchMe() {
  if (lastMeTry && millis() - lastMeTry < 10000) return;
  lastMeTry = millis();
  needMe = false;
  String body;
  if (api("GET", "/me", "", &body) != 200) return;
  JsonDocument doc(&spiAlloc);
  if (deserializeJson(doc, body)) return;
  LOCK();
  copyStr(myId, sizeof(myId), doc["id"] | "");
  copyFold(myName, sizeof(myName), doc["display_name"] | myId);
  copyStr(myAvatar, sizeof(myAvatar), pickImage(doc["images"], 64).c_str());
  UNLOCK();
  if (myAvatar[0]) needAvatar = true;
  saveStr("uid", myId);
  saveStr("uname", myName);
  Serial.printf("[auth] logged in as %s (%s)\n", myName, myId);
}

void loadPlaylists() {
  needPlaylists = false;
  plLoading = true;
  JsonDocument filter(&spiAlloc);
  JsonObject fi = filter["items"][0].to<JsonObject>();
  fi["id"] = true;
  fi["name"] = true;
  fi["owner"]["id"] = true;
  fi["owner"]["display_name"] = true;
  fi["images"][0]["url"] = true;
  fi["images"][0]["width"] = true;
  fi["items"]["total"] = true;
  fi["tracks"]["total"] = true;
  filter["next"] = true;

  int n = 0, off = 0;
  while (n < MAX_PL) {
    String body;
    int code = api("GET", "/me/playlists?limit=50&offset=" + String(off), "", &body);
    if (code != 200) break;
    JsonDocument doc(&spiAlloc);
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) break;
    JsonArray arr = doc["items"];
    for (JsonObject o : arr) {
      if (n >= MAX_PL) break;
      if (o.isNull() || !(o["id"] | "")[0]) continue;
      PL p;
      memset(&p, 0, sizeof(p));
      copyStr(p.id, sizeof(p.id), o["id"] | "");
      copyFold(p.name, sizeof(p.name), o["name"] | "Untitled");
      if (!p.name[0]) copyStr(p.name, sizeof(p.name), "Untitled");
      p.total = o["items"]["total"] | (o["tracks"]["total"] | 0);
      const char* oid = o["owner"]["id"] | "";
      p.own = myId[0] && strcmp(oid, myId) == 0;
      char owner[40];
      copyFold(owner, sizeof(owner), o["owner"]["display_name"] | oid);
      copyStr(p.sub, sizeof(p.sub), p.own ? "You" : owner);
      copyStr(p.img, sizeof(p.img), pickImage(o["images"], 300).c_str());
      LOCK();
      if (n >= plCount || strcmp(pls[n].id, p.id) != 0 || strcmp(pls[n].img, p.img) != 0) thumbState[n] = 0;
      pls[n] = p;
      n++;
      if (n > plCount) plCount = n;
      UNLOCK();
    }
    if (doc["next"].isNull() || arr.size() == 0) break;
    off += 50;
  }
  LOCK();
  plCount = n;
  UNLOCK();
  plLoading = false;
  plLoadedOnce = true;
  Serial.printf("[lib] %d playlists\n", n);
}

void loadTracks(const char* id, int offset, uint16_t gen) {
  bool liked = strcmp(id, "liked") == 0;
  bool album = strncmp(id, "album:", 6) == 0;
  String path = liked ? "/me/tracks?limit=50&offset=" + String(offset)
              : album ? "/albums/" + String(id + 6) + "/tracks?limit=50&offset=" + String(offset)
                      : "/playlists/" + String(id) + "/items?limit=50&offset=" + String(offset) + "&additional_types=track,episode";
  bool plain = liked || album;
  String slim = path + "&fields=" + urlEnc("total,items(item(uri,name,duration_ms,type,artists(name),show(name)))");
  trLoading = true;
  String body;
  int code = api("GET", plain ? path : slim, "", &body);
  if (!plain && code == 400) code = api("GET", path, "", &body);
  Serial.printf("[list] %s offset %d -> %d (%u bytes)\n", id, offset, code, (unsigned)body.length());
  if (code != 200) {
    if (gen == trGen) trError = code;
    trLoading = false;
    return;
  }
  JsonDocument filter(&spiAlloc);
  JsonObject fi = filter["items"][0].to<JsonObject>();
  for (const char* k : {"item", "track"}) {
    JsonObject t = fi[k].to<JsonObject>();
    t["uri"] = true;
    t["name"] = true;
    t["duration_ms"] = true;
    t["artists"][0]["name"] = true;
    t["show"]["name"] = true;
  }
  fi["uri"] = true;
  fi["name"] = true;
  fi["duration_ms"] = true;
  fi["artists"][0]["name"] = true;
  filter["total"] = true;
  JsonDocument doc(&spiAlloc);
  DeserializationError je = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (je) {
    Serial.printf("[list] JSON error %s\n", je.c_str());
    if (gen == trGen) trError = -2;
    trLoading = false;
    return;
  }

  LOCK();
  if (gen != trGen) { UNLOCK(); trLoading = false; return; }
  trTotal = doc["total"] | 0;
  for (JsonObject o : doc["items"].as<JsonArray>()) {
    if (trCount >= MAX_TR) break;
    JsonObject t = o["item"].is<JsonObject>() ? o["item"].as<JsonObject>() : o["track"].is<JsonObject>() ? o["track"].as<JsonObject>() : o;
    TR r;
    memset(&r, 0, sizeof(r));
    if (!t.isNull()) {
      copyStr(r.uri, sizeof(r.uri), t["uri"] | "");
      copyFold(r.name, sizeof(r.name), t["name"] | "");
      String a;
      for (JsonObject ar : t["artists"].as<JsonArray>()) {
        if (a.length()) a += ", ";
        a += (const char*)(ar["name"] | "");
      }
      if (!a.length()) a = (const char*)(t["show"]["name"] | "");
      copyFold(r.artist, sizeof(r.artist), a.c_str());
      r.dur = t["duration_ms"] | 0;
    }
    if (!r.name[0]) copyStr(r.name, sizeof(r.name), "Unavailable");
    trs[trCount] = r;
    trCount = trCount + 1;
  }
  if (offset == 0 && trCount == 0 && trTotal > 0) trError = 403;
  else trError = 0;
  UNLOCK();
  trLoading = false;
  Serial.printf("[list] %s: %d / %d loaded\n", id, trCount, trTotal);
}

void pollPlayer() {
  String body;
  int code = api("GET", "/me/player?additional_types=episode", "", &body);
  uint32_t now = millis();
  if (code != lastPlayerCode) Serial.printf("[now] player status %d\n", code);
  lastPlayerCode = code;
  if (code == 204 || code == 202) {
    failPolls = 0;
    lastGoodPoll = now;
    if (++emptyPolls < 3) { pollNow = true; return; }
    LOCK();
    shared.active = false;
    if ((int32_t)(holdPlayUntil - now) <= 0) shared.playing = false;
    UNLOCK();
    return;
  }
  if (code != 200) {
    if (failPolls < 10) failPolls++;
    return;
  }
  failPolls = 0;
  lastGoodPoll = now;

  JsonDocument filter(&spiAlloc);
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["shuffle_state"] = true;
  filter["repeat_state"] = true;
  filter["currently_playing_type"] = true;
  filter["device"]["name"] = true;
  filter["device"]["volume_percent"] = true;
  filter["device"]["supports_volume"] = true;
  JsonObject it = filter["item"].to<JsonObject>();
  it["id"] = true;
  it["uri"] = true;
  it["name"] = true;
  it["duration_ms"] = true;
  it["type"] = true;
  it["artists"][0]["name"] = true;
  it["album"]["name"] = true;
  it["album"]["images"][0]["url"] = true;
  it["album"]["images"][0]["width"] = true;
  it["show"]["name"] = true;
  it["images"][0]["url"] = true;
  it["images"][0]["width"] = true;
  filter["context"]["uri"] = true;

  JsonDocument doc(&spiAlloc);
  DeserializationError je = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (je) { Serial.printf("[now] JSON error %s (%u bytes)\n", je.c_str(), (unsigned)body.length()); return; }

  Player p;
  p.active = true;
  p.playing = doc["is_playing"] | false;
  p.progress = doc["progress_ms"] | 0;
  p.fetchedAt = now;
  p.shuffle = doc["shuffle_state"] | false;
  const char* rep = doc["repeat_state"] | "off";
  p.repeat = strcmp(rep, "context") == 0 ? 1 : strcmp(rep, "track") == 0 ? 2 : 0;
  p.volume = doc["device"]["volume_percent"].isNull() ? -1 : (int)(doc["device"]["volume_percent"] | 0);
  p.supportsVolume = doc["device"]["supports_volume"] | true;
  copyFold(p.device, sizeof(p.device), doc["device"]["name"] | "");
  copyStr(p.context, sizeof(p.context), doc["context"]["uri"] | "");

  JsonObject item = doc["item"];
  if (!item.isNull()) {
    p.isTrack = strcmp(item["type"] | "track", "episode") != 0;
    copyStr(p.trackId, sizeof(p.trackId), item["id"] | "");
    copyStr(p.uri, sizeof(p.uri), item["uri"] | "");
    copyFold(p.title, sizeof(p.title), item["name"] | "");
    p.duration = item["duration_ms"] | 0;
    if (p.isTrack) {
      String a;
      for (JsonObject ar : item["artists"].as<JsonArray>()) {
        if (a.length()) a += ", ";
        a += (const char*)(ar["name"] | "");
      }
      copyFold(p.artist, sizeof(p.artist), a.c_str());
      copyFold(p.album, sizeof(p.album), item["album"]["name"] | "");
      copyStr(p.art, sizeof(p.art), pickImage(item["album"]["images"], 300).c_str());
    } else {
      copyFold(p.artist, sizeof(p.artist), item["show"]["name"] | "");
      copyStr(p.album, sizeof(p.album), "Podcast");
      copyStr(p.art, sizeof(p.art), pickImage(item["images"], 300).c_str());
    }
  } else if (strcmp(doc["currently_playing_type"] | "", "ad") == 0) {
    copyStr(p.title, sizeof(p.title), "Advertisement");
  }

  if (!p.title[0] && strcmp(doc["currently_playing_type"] | "", "ad") != 0) {
    if (++emptyPolls < 3) { pollNow = true; return; }
  } else {
    emptyPolls = 0;
  }

  LOCK();
  if ((int32_t)(holdTrackUntil - now) > 0) {
    bool stale = (wantUri[0] && strcmp(p.uri, wantUri) != 0) || (skipFromUri[0] && strcmp(p.uri, skipFromUri) == 0);
    if (stale) { UNLOCK(); pollNow = true; return; }
  }
  wantUri[0] = skipFromUri[0] = 0;
  holdTrackUntil = 0;
  if (p.playing == shared.playing) holdPlayUntil = 0;
  if (p.shuffle == shared.shuffle) holdShuffleUntil = 0;
  if (p.repeat == shared.repeat) holdRepeatUntil = 0;
  bool trackChanged = strcmp(p.trackId, shared.trackId) != 0;
  if ((int32_t)(holdPlayUntil - now) > 0) p.playing = shared.playing;
  if ((int32_t)(holdShuffleUntil - now) > 0) p.shuffle = shared.shuffle;
  if ((int32_t)(holdRepeatUntil - now) > 0) p.repeat = shared.repeat;
  if ((int32_t)(holdVolUntil - now) > 0 || volDirty) p.volume = shared.volume;
  if (!trackChanged && (int32_t)(holdSeekUntil - now) > 0) { p.progress = shared.progress; p.fetchedAt = shared.fetchedAt; }
  if (!trackChanged) p.liked = shared.liked;
  if (trackChanged) Serial.printf("[now] %s - %s\n", p.title, p.artist);
  shared = p;
  if (trackChanged && p.isTrack && p.trackId[0]) needLikeCheck = true;
  UNLOCK();
}

void checkLiked() {
  needLikeCheck = false;
  char uri[64];
  LOCK();
  copyStr(uri, sizeof(uri), shared.uri);
  UNLOCK();
  if (!uri[0] || strncmp(uri, "spotify:track:", 14) != 0) return;
  String body;
  if (api("GET", "/me/library/contains?uris=" + urlEnc(uri), "", &body) != 200) return;
  JsonDocument doc(&spiAlloc);
  if (deserializeJson(doc, body)) return;
  bool liked = doc[0] | false;
  LOCK();
  if (strcmp(uri, shared.uri) == 0 && (int32_t)(holdLikeUntil - millis()) <= 0) shared.liked = liked;
  UNLOCK();
}

void fetchArtIfNeeded() {
  if (artPending) return;
  char url[200];
  LOCK();
  copyStr(url, sizeof(url), shared.art);
  UNLOCK();
  if (!url[0] || strcmp(url, artFetchedUrl) == 0) return;
  copyStr(artFetchedUrl, sizeof(artFetchedUrl), url);
  int n = download(url, artBuf, 200000);
  if (n > 0) {
    artBufLen = n;
    artPending = true;
  }
}

const char* thumbUrlLocked(int slot) {
  if (slot < 0) return "";
  if (slot < MAX_PL) return slot < plCount ? pls[slot].img : "";
  int r = slot - MAX_PL;
  return r < recentCount ? recent[r].img : "";
}

void loadRecent() {
  needRecent = false;
  recentState = RS_LOADING;
  String body;
  int code = api("GET", "/me/player/recently-played?limit=50", "", &body);
  Serial.printf("[recent] -> %d\n", code);
  if (code == 401 || code == 403) { recentState = RS_SCOPE; recentAt = millis(); return; }
  if (code != 200) { recentState = RS_ERROR; recentAt = millis(); return; }
  JsonDocument filter(&spiAlloc);
  JsonObject fi = filter["items"][0].to<JsonObject>();
  fi["context"]["uri"] = true;
  fi["context"]["type"] = true;
  JsonObject al = fi["track"]["album"].to<JsonObject>();
  al["uri"] = true;
  al["name"] = true;
  al["images"][0]["url"] = true;
  al["images"][0]["width"] = true;
  al["artists"][0]["name"] = true;
  JsonDocument doc(&spiAlloc);
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { recentState = RS_ERROR; return; }
  RecentItem got[6];
  int n = 0;
  for (JsonObject it : doc["items"].as<JsonArray>()) {
    if (n >= 6) break;
    RecentItem r;
    memset(&r, 0, sizeof(r));
    r.plIdx = -1;
    const char* cu = it["context"]["uri"] | "";
    const char* ct = it["context"]["type"] | "";
    if (strcmp(ct, "playlist") == 0 && strncmp(cu, "spotify:playlist:", 17) == 0) {
      r.kind = LK_PLAYLIST;
      copyStr(r.key, sizeof(r.key), cu + 17);
    } else if (strcmp(ct, "collection") == 0 || strstr(cu, ":collection")) {
      r.kind = LK_LIKED;
      copyStr(r.key, sizeof(r.key), "liked");
      copyStr(r.name, sizeof(r.name), "Liked Songs");
      copyStr(r.owner, sizeof(r.owner), "You");
    } else {
      const char* au = it["track"]["album"]["uri"] | "";
      if (strncmp(au, "spotify:album:", 14) != 0) continue;
      r.kind = LK_ALBUM;
      snprintf(r.key, sizeof(r.key), "album:%s", au + 14);
      copyFold(r.name, sizeof(r.name), it["track"]["album"]["name"] | "Album");
      copyFold(r.owner, sizeof(r.owner), it["track"]["album"]["artists"][0]["name"] | "");
      copyStr(r.img, sizeof(r.img), pickImage(it["track"]["album"]["images"], 300).c_str());
    }
    bool dup = false;
    for (int j = 0; j < n; j++) if (strcmp(got[j].key, r.key) == 0) dup = true;
    if (!dup) got[n++] = r;
  }
  for (int i = 0; i < n; i++) {
    if (got[i].kind != LK_PLAYLIST) continue;
    LOCK();
    for (int k = 0; k < plCount; k++) {
      if (strcmp(pls[k].id, got[i].key) != 0) continue;
      got[i].plIdx = k;
      copyStr(got[i].name, sizeof(got[i].name), pls[k].name);
      copyStr(got[i].owner, sizeof(got[i].owner), pls[k].sub);
      copyStr(got[i].img, sizeof(got[i].img), pls[k].img);
      got[i].total = pls[k].total;
      break;
    }
    UNLOCK();
    if (got[i].plIdx >= 0) continue;
    String pb;
    String path = "/playlists/" + String(got[i].key) + "?fields=" + urlEnc("name,owner(display_name),images,items(total)");
    if (api("GET", path, "", &pb) == 200) {
      JsonDocument pd(&spiAlloc);
      if (!deserializeJson(pd, pb)) {
        copyFold(got[i].name, sizeof(got[i].name), pd["name"] | "Playlist");
        copyFold(got[i].owner, sizeof(got[i].owner), pd["owner"]["display_name"] | "");
        copyStr(got[i].img, sizeof(got[i].img), pickImage(pd["images"], 300).c_str());
        got[i].total = pd["items"]["total"] | 0;
      }
    }
  }
  int m = 0;
  for (int i = 0; i < n; i++) if (got[i].name[0]) got[m++] = got[i];
  LOCK();
  for (int i = 0; i < m; i++) {
    if (i >= recentCount || strcmp(recent[i].img, got[i].img) != 0) thumbState[MAX_PL + i] = 0;
    recent[i] = got[i];
  }
  recentCount = m;
  UNLOCK();
  recentState = RS_OK;
  recentAt = millis();
  Serial.printf("[recent] %d items\n", m);
}

void fetchThumbIfNeeded() {
  if (thumbBufIdx >= 0) return;
  int idx = -1;
  char url[200];
  LOCK();
  for (int i = 0; i < NTHUMB; i++) {
    if (thumbState[i] == 1) { idx = i; break; }
  }
  if (idx >= 0) {
    thumbState[idx] = 2;
    copyStr(url, sizeof(url), thumbUrlLocked(idx));
  }
  UNLOCK();
  if (idx < 0) return;
  int n = url[0] ? download(url, thumbBuf, 150000) : 0;
  if (n > 0) {
    thumbBufLen = n;
    thumbBufIdx = idx;
    thumbState[idx] = 3;
  } else {
    thumbState[idx] = 5;
  }
}

void fetchAvatarIfNeeded() {
  if (!needAvatar || avatarPending || !avatarBuf) return;
  needAvatar = false;
  char url[200];
  LOCK();
  copyStr(url, sizeof(url), myAvatar);
  UNLOCK();
  if (!url[0]) return;
  int n = download(url, avatarBuf, 60000);
  if (n > 0) { avatarBufLen = n; avatarPending = true; }
}

void sendVolumeIfDue() {
  if (!volDirty) return;
  uint32_t now = millis();
  if (now - volChangedAt < 150 || now - lastVolSent < 450) return;
  int v = volTarget;
  volDirty = false;
  lastVolSent = now;
  LOCK();
  holdVolUntil = now + 2500;
  UNLOCK();
  int code = api("PUT", "/me/player/volume?volume_percent=" + String(v));
  Serial.printf("[vol] set %d -> %d\n", v, code);
  if (code == 404) setToast("Nothing is playing right now");
  else if (code == 403) setToast("This device doesn't allow volume control");
}

void runCmd(const Cmd& c) {
  switch (c.type) {
    case C_TOGGLE: {
      if (c.a) playerCall("PUT", "/me/player/play");
      else playerCall("PUT", "/me/player/pause");
      break;
    }
    case C_NEXT: playerCall("POST", "/me/player/next"); break;
    case C_PREV: playerCall("POST", "/me/player/previous"); break;
    case C_SHUFFLE: playerCall("PUT", String("/me/player/shuffle?state=") + (c.a ? "true" : "false")); break;
    case C_REPEAT: {
      const char* m = c.a == 1 ? "context" : c.a == 2 ? "track" : "off";
      playerCall("PUT", String("/me/player/repeat?state=") + m);
      break;
    }
    case C_SEEK: playerCall("PUT", "/me/player/seek?position_ms=" + String(c.a)); break;
    case C_LIKE: {
      String u = urlEnc(c.s);
      int code = api(c.a ? "PUT" : "DELETE", "/me/library?uris=" + u);
      if (code >= 200 && code < 300) {
        char pl[28], pln[64];
        LOCK();
        copyStr(pl, sizeof(pl), likePl);
        copyStr(pln, sizeof(pln), likePlName);
        UNLOCK();
        if (pl[0]) {
          if (c.a) api("POST", "/playlists/" + String(pl) + "/items", "{\"uris\":[\"" + jsonEsc(c.s) + "\"]}");
          else api("DELETE", "/playlists/" + String(pl) + "/items", "{\"items\":[{\"uri\":\"" + jsonEsc(c.s) + "\"}]}");
        }
        char m[96];
        if (c.a) snprintf(m, sizeof(m), pl[0] ? "Added to Liked Songs and %s" : "Added to Liked Songs", pln);
        else snprintf(m, sizeof(m), "Removed from Liked Songs");
        setToast(m);
      } else {
        LOCK();
        shared.liked = !c.a;
        UNLOCK();
        setToast("Couldn't update Liked Songs");
      }
      break;
    }
    case C_OPEN_LIST:
    case C_MORE: {
      uint16_t gen = (uint16_t)c.b;
      if (gen != trGen) break;
      loadTracks(c.s, c.a, gen);
      break;
    }
    case C_PLAY_AT: {
      bool liked = strcmp(c.s, "liked") == 0;
      if (c.b) {
        playerCall("PUT", "/me/player/shuffle?state=true");
        LOCK();
        shared.shuffle = true;
        holdShuffleUntil = millis() + 3000;
        UNLOCK();
      }
      String body;
      if (liked) {
        LOCK();
        int start = max(0, (int)c.a - 50);
        int cnt = min(100, trCount - start);
        body = "{\"uris\":[";
        int added = 0, pos = 0;
        for (int i = start; i < start + cnt; i++) {
          if (!trs[i].uri[0] || strncmp(trs[i].uri, "spotify:local", 13) == 0) continue;
          if (i == c.a) pos = added;
          if (added) body += ",";
          body += "\"" + String(trs[i].uri) + "\"";
          added++;
        }
        body += "]";
        if (!c.b) body += ",\"offset\":{\"position\":" + String(pos) + "}";
        body += "}";
        UNLOCK();
        if (!added) break;
      } else {
        if (strncmp(c.s, "album:", 6) == 0) body = "{\"context_uri\":\"spotify:album:" + String(c.s + 6) + "\"";
        else body = "{\"context_uri\":\"spotify:playlist:" + String(c.s) + "\"";
        if (!c.b) body += ",\"offset\":{\"position\":" + String(c.a) + "},\"position_ms\":0";
        body += "}";
      }
      int code = playerCall("PUT", "/me/player/play", body);
      Serial.printf("[play] %s #%d -> %d\n", c.s, (int)c.a, code);
      break;
    }
    case C_RELOAD_PL: loadPlaylists(); break;
  }
}

void netTask(void*) {
  apiClient.setInsecure();
  imgClient.setInsecure();
  apiClient.setHandshakeTimeout(12);
  imgClient.setHandshakeTimeout(12);
  uint32_t nextPoll = 0, nextBeat = 0, lastPoll = 0, lastHeal = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    if (!timeStarted) {
      configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
      timeStarted = true;
    }
    if (!loggedIn()) { vTaskDelay(pdMS_TO_TICKS(300)); continue; }
    if (needMe || !myId[0] || (!myAvatar[0] && !lastMeTry)) fetchMe();
    if (needPlaylists) loadPlaylists();
    if (needRecent && plLoadedOnce) loadRecent();

    Cmd c;
    bool did = false;
    int handled = 0;
    while (handled++ < 4 && xQueueReceive(cmdQ, &c, 0) == pdTRUE) {
      runCmd(c);
      if (c.type != C_OPEN_LIST && c.type != C_MORE && c.type != C_RELOAD_PL) did = true;
    }
    uint32_t now = millis();
    if (did && (int32_t)(nextPoll - (now + 400)) > 0) nextPoll = now + 400;
    if (pollNow) { pollNow = false; if ((int32_t)(nextPoll - (now + 300)) > 0) nextPoll = now + 300; }

    LOCK();
    bool ended = shared.playing && shared.duration && shared.progress + (now - shared.fetchedAt) > shared.duration + 600;
    UNLOCK();
    if (ended && now - lastPoll > 1200) nextPoll = now;

    sendVolumeIfDue();

    if ((int32_t)(millis() - nextPoll) >= 0 && millis() - lastPoll >= 300) {
      lastPoll = millis();
      pollPlayer();
      uint32_t iv;
      if (failPolls) iv = min(8000u, 1000u << min((int)failPolls, 3));
      else if (screenAsleep) iv = 5000;
      else if (shared.playing) iv = 1200;
      else iv = 2500;
      nextPoll = millis() + iv;
    }

    if (lastGoodPoll && millis() - lastGoodPoll > 20000 && millis() - lastHeal > 20000) {
      lastHeal = millis();
      Serial.printf("[sync] no good reply for %us, resetting connection\n", (unsigned)((millis() - lastGoodPoll) / 1000));
      apiClient.stop();
      accessToken[0] = 0;
      rateLimitedUntil = 0;
      nextPoll = millis();
    }
    if (needLikeCheck) checkLiked();
    if ((int32_t)(millis() - nextBeat) >= 0) {
      nextBeat = millis() + 30000;
      Serial.printf("[net] ok, player %d, last good reply %us ago, internal heap %u (largest %u), psram %u\n", lastPlayerCode,
                    (unsigned)(lastGoodPoll ? (millis() - lastGoodPoll) / 1000 : 0),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
    if (!screenAsleep) {
      fetchArtIfNeeded();
      fetchThumbIfNeeded();
      fetchAvatarIfNeeded();
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ---------------------------------------------------------------- web setup page

const char PAGE_HEAD[] PROGMEM = R"(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>Car Thing setup</title>
<style>
body{margin:0;background:#121212;color:#fff;font-family:-apple-system,Segoe UI,Roboto,sans-serif;padding:20px;max-width:560px;margin:auto}
h1{font-size:24px;margin:8px 0 20px}.card{background:#1f1f1f;border-radius:12px;padding:16px;margin-bottom:14px}
.step{color:#1ed760;font-weight:700;font-size:13px;letter-spacing:.08em;text-transform:uppercase}
p{color:#b3b3b3;line-height:1.45}code{display:block;background:#000;padding:10px;border-radius:8px;color:#1ed760;word-break:break-all;user-select:all}
.btn,button{display:inline-block;background:#1ed760;color:#000;font-weight:700;border:0;border-radius:999px;padding:12px 22px;text-decoration:none;font-size:15px;cursor:pointer}
.ghost{background:#333;color:#fff}textarea,select{width:100%;box-sizing:border-box;background:#000;color:#fff;border:1px solid #444;border-radius:8px;padding:10px;font-size:14px;margin:6px 0 12px}
textarea{height:90px}.ok{color:#1ed760}.err{color:#ff6b6b}
</style></head><body>)";

String pageStatus(const String& msg, bool good) {
  if (!msg.length()) return "";
  return String("<div class=card><b class=") + (good ? "ok" : "err") + ">" + msg + "</b></div>";
}

void handleRoot(const String& msg = "", bool good = true) {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Car Thing</h1>";
  h += pageStatus(msg, good);
  if (!loggedIn()) {
    h += "<div class=card><div class=step>Step 1</div><p>In the Spotify Developer Dashboard, open your app's settings and add this Redirect URI exactly, then save:</p><code>" REDIRECT_URI "</code></div>";
    h += "<div class=card><div class=step>Step 2</div><p>Log in to Spotify and tap <b>Agree</b>.</p><a class=btn href=/login target=_blank>Log in with Spotify</a></div>";
    h += "<div class=card><div class=step>Step 3</div><p>Spotify then opens a page that won't load. That's expected. Copy the whole address from the address bar and paste it here.</p>"
         "<form method=post action=/finish><textarea name=u placeholder='http://127.0.0.1:8888/callback?code=...'></textarea><button>Connect</button></form></div>";
  } else {
    LOCK();
    h += "<div class=card><div class=step>Connected</div><p>Logged in as <b style=color:#fff>" + htmlEsc(myName[0] ? myName : "your account") + "</b>. The device remembers this login after a restart.</p></div>";
    UNLOCK();
    h += "<div class=card><form method=post action=/logout><button class='ghost'>Log out</button></form></div>";
  }
  h += "</body></html>";
  server.send(200, "text/html", h);
}

void handleLogin() {
  server.sendHeader("Location", authorizeUrl());
  server.send(302, "text/plain", "");
}

void handleFinish() {
  String u = server.arg("u");
  u.trim();
  String code = u;
  int ci = u.indexOf("code=");
  if (ci >= 0) {
    code = u.substring(ci + 5);
    int amp = code.indexOf('&');
    if (amp >= 0) code = code.substring(0, amp);
  }
  code = urlDec(code);
  if (u.indexOf("error=") >= 0) { handleRoot("Spotify said access was denied. Try again.", false); return; }
  int si = u.indexOf("state=");
  if (si >= 0 && authState[0]) {
    String st = u.substring(si + 6);
    int amp = st.indexOf('&');
    if (amp >= 0) st = st.substring(0, amp);
    if (st != authState) { handleRoot("That link is from an older login attempt. Tap Log in again.", false); return; }
  }
  if (code.length() < 20) { handleRoot("That doesn't look like the right address. It should contain code=", false); return; }
  if (exchangeCode(code)) handleRoot("Connected! Your Car Thing is ready.", true);
  else handleRoot("Login failed. Check the Client ID, Client Secret and Redirect URI, then try again.", false);
}

void handleLogout() {
  logout();
  handleRoot("Logged out.", true);
}

void webTask(void*) {
  server.on("/", HTTP_GET, []() { handleRoot(); });
  server.on("/login", HTTP_GET, handleLogin);
  server.on("/finish", HTTP_POST, handleFinish);
  server.on("/logout", HTTP_POST, handleLogout);
  server.onNotFound([]() { server.sendHeader("Location", "/"); server.send(302, "text/plain", ""); });
  bool started = false;
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!started) {
        server.begin();
        MDNS.begin(DEVICE_HOSTNAME);
        MDNS.addService("http", "tcp", 80);
        started = true;
        Serial.printf("[web] setup page at http://%s or http://%s.local\n", WiFi.localIP().toString().c_str(), DEVICE_HOSTNAME);
      }
      server.handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(3));
  }
}

// ---------------------------------------------------------------- encoder

void encoderTask(void*) {
  static const int8_t T[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t last = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  for (;;) {
    uint8_t s = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
    if (s != last) {
      encRaw = encRaw + T[(last << 2) | s];
      last = s;
    }
    vTaskDelay(1);
  }
}

// ---------------------------------------------------------------- drawing helpers

uint16_t mix(uint16_t a, uint16_t b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  int ar = (a >> 8) & 0xF8, ag = (a >> 3) & 0xFC, ab = (a << 3) & 0xF8;
  int br = (b >> 8) & 0xF8, bg = (b >> 3) & 0xFC, bb = (b << 3) & 0xF8;
  return RGB565((uint8_t)(br + (ar - br) * t), (uint8_t)(bg + (ag - bg) * t), (uint8_t)(bb + (ab - bb) * t));
}

float easeOutCubic(float t) { t = constrain(t, 0.0f, 1.0f); return 1 - powf(1 - t, 3); }
float easeOutBack(float t) {
  t = constrain(t, 0.0f, 1.0f);
  const float c1 = 1.70158f, c3 = c1 + 1;
  return 1 + c3 * powf(t - 1, 3) + c1 * powf(t - 1, 2);
}

#define ELLIPSIS "\xE2\x80\xA6"
#define MIDDOT   "\xC2\xB7"

void fitText(char* out, size_t cap, const char* s, int maxW) {
  size_t n = strlen(s);
  if (n + 4 > cap) n = cap - 4;
  memcpy(out, s, n);
  out[n] = 0;
  if (spr.textWidth(out) <= maxW) return;
  while (n > 0) {
    n--;
    while (n > 0 && ((uint8_t)out[n] & 0xC0) == 0x80) n--;
    while (n > 0 && out[n - 1] == ' ') n--;
    strcpy(out + n, ELLIPSIS);
    if (spr.textWidth(out) <= maxW) return;
  }
}

void txt(const char* s, int x, int y, const lgfx::IFont* f, uint16_t c, textdatum_t d = textdatum_t::top_left, int maxW = 0) {
  spr.setFont(f);
  spr.setTextColor(c);
  spr.setTextDatum(d);
  if (maxW > 0 && spr.textWidth(s) > maxW) {
    char buf[160];
    fitText(buf, sizeof(buf), s, maxW);
    spr.drawString(buf, x, y);
  } else {
    spr.drawString(s, x, y);
  }
}

int textW(const char* s, const lgfx::IFont* f) {
  spr.setFont(f);
  return spr.textWidth(s);
}

float advOf(const lgfx::IFont* f, char ch) {
  auto v = (const lgfx::VLWfont*)f;
  uint16_t gi = 0;
  if (v->getUnicodeIndex((uint8_t)ch, &gi)) return v->gxAdvance[gi];
  return v->spaceWidth;
}

void capsTxt(const char* s, int x, int y, const lgfx::IFont* f, uint16_t col, float gap, int maxW) {
  auto v = (const lgfx::VLWfont*)f;
  spr.setFont(f);
  spr.setTextColor(col);
  spr.setTextDatum(textdatum_t::baseline_left);
  char up[96];
  int n = 0;
  for (; s[n] && n < 95; n++) up[n] = toupper((unsigned char)s[n]);
  up[n] = 0;
  auto step = [&](char ch, int& dx) -> float {
    uint16_t gi = 0;
    dx = 0;
    if (ch == ' ' || !v->getUnicodeIndex((uint8_t)ch, &gi)) return v->spaceWidth + gap;
    dx = v->gdX[gi];
    return v->gWidth[gi] + gap;
  };
  auto width = [&](int len) { float w = 0; int d; for (int i = 0; i < len; i++) w += step(up[i], d); return w - gap; };
  bool cut = false;
  int ew = spr.textWidth(ELLIPSIS);
  while (n > 0 && width(n) + (cut ? ew + gap : 0) > maxW) { n--; cut = true; }
  int base = y + (v->maxAscent - v->maxDescent) / 2;
  float bx = x;
  char c[2] = {0, 0};
  for (int i = 0; i < n; i++) {
    int dx;
    float w = step(up[i], dx);
    c[0] = up[i];
    if (up[i] != ' ') spr.drawString(c, (int)(bx + 0.5f) - dx, base);
    bx += w;
  }
  if (cut) spr.drawString(ELLIPSIS, (int)(bx + 0.5f), base);
}

uint32_t marqueeStart = 0;

void fmtTime(char* b, uint32_t ms) {
  uint32_t s = ms / 1000;
  sprintf(b, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

bool fmtClock(char* b, size_t n) {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return false;
  int h = ti.tm_hour;
  if (!clock24) { h %= 12; if (h == 0) h = 12; }
  snprintf(b, n, "%d:%02d", h, ti.tm_min);
  return true;
}

bool fmtDate(char* b, size_t n) {
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return false;
  char dw[16], mo[16];
  strftime(dw, sizeof(dw), "%A", &ti);
  strftime(mo, sizeof(mo), "%B", &ti);
  snprintf(b, n, "%s, %d %s", dw, ti.tm_mday, mo);
  return true;
}

// ---- antialiased shapes

void blendPx(int x, int y, uint16_t c, float a) {
  if (x < 0 || y < 0 || x >= 320 || y >= 240 || a <= 0.01f) return;
  if (a >= 0.99f) { spr.drawPixel(x, y, c); return; }
  auto p = spr.readPixelRGB(x, y);
  spr.drawPixel(x, y, mix(c, RGB565(p.r, p.g, p.b), a));
}

void aaPaint(float x0, float y0, float x1, float y1, uint16_t c, std::function<bool(float, float)> in) {
  int ix0 = (int)floorf(x0), iy0 = (int)floorf(y0), ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);
  for (int y = iy0; y <= iy1; y++) {
    for (int x = ix0; x <= ix1; x++) {
      int n = 0;
      for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++)
          if (in(x + (sx + 0.5f) * 0.25f, y + (sy + 0.5f) * 0.25f)) n++;
      if (n) blendPx(x, y, c, n / 16.0f);
    }
  }
}

float segDist(float px, float py, float ax, float ay, float bx, float by) {
  float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
  float t = constrain((wx * vx + wy * vy) / (vx * vx + vy * vy + 1e-6f), 0.0f, 1.0f);
  float dx = wx - vx * t, dy = wy - vy * t;
  return sqrtf(dx * dx + dy * dy);
}

void aaLine(float ax, float ay, float bx, float by, float w, uint16_t c) {
  float r = w / 2;
  aaPaint(min(ax, bx) - r - 1, min(ay, by) - r - 1, max(ax, bx) + r + 1, max(ay, by) + r + 1, c,
          [=](float x, float y) { return segDist(x, y, ax, ay, bx, by) <= r; });
}

void aaCircle(float cx, float cy, float r, uint16_t c) {
  aaPaint(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, c,
          [=](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; });
}

bool angleIn(float a, float a0, float a1) {
  if (a1 - a0 >= 360) return true;
  while (a < a0) a += 360;
  while (a >= a0 + 360) a -= 360;
  return a <= a1;
}

void aaArc(float cx, float cy, float r, float w, float a0, float a1, uint16_t c) {
  float o = r + w / 2 + 1;
  aaPaint(cx - o, cy - o, cx + o, cy + o, c, [=](float x, float y) {
    float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
    if (fabsf(d - r) > w / 2) return false;
    return angleIn(atan2f(y - cy, x - cx) * 57.29578f, a0, a1);
  });
}

void aaTri(float ax, float ay, float bx, float by, float qx, float qy, uint16_t c) {
  auto side = [](float px, float py, float x0, float y0, float x1, float y1) { return (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0); };
  aaPaint(min(ax, min(bx, qx)) - 1, min(ay, min(by, qy)) - 1, max(ax, max(bx, qx)) + 1, max(ay, max(by, qy)) + 1, c, [=](float x, float y) {
    float d1 = side(x, y, ax, ay, bx, by), d2 = side(x, y, bx, by, qx, qy), d3 = side(x, y, qx, qy, ax, ay);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
  });
}

void aaRRect(float x, float y, float w, float h, float r, uint16_t c) {
  aaPaint(x - 1, y - 1, x + w + 1, y + h + 1, c, [=](float px, float py) {
    if (px < x || px > x + w || py < y || py > y + h) return false;
    float qx = px < x + r ? x + r : px > x + w - r ? x + w - r : px;
    float qy = py < y + r ? y + r : py > y + h - r ? y + h - r : py;
    return (px - qx) * (px - qx) + (py - qy) * (py - qy) <= r * r;
  });
}

void roundCorners(int x, int y, int w, int h, int r, uint16_t bg, uint8_t which = 15) {
  if (r <= 0) return;
  for (int k = 0; k < 4; k++) {
    if (!(which & (1 << k))) continue;
    bool right = k == 1 || k == 3, bottom = k >= 2;
    float ccx = right ? x + w - r : x + r, ccy = bottom ? y + h - r : y + r;
    int bx = right ? x + w - r : x, by = bottom ? y + h - r : y;
    for (int py = by; py < by + r; py++) {
      for (int px = bx; px < bx + r; px++) {
        int n = 0;
        for (int sy = 0; sy < 4; sy++)
          for (int sx = 0; sx < 4; sx++) {
            float fx = px + (sx + 0.5f) * 0.25f, fy = py + (sy + 0.5f) * 0.25f;
            bool outX = right ? fx > ccx : fx < ccx, outY = bottom ? fy > ccy : fy < ccy;
            if (outX && outY && (fx - ccx) * (fx - ccx) + (fy - ccy) * (fy - ccy) > r * r) n++;
          }
        if (n) blendPx(px, py, bg, n / 16.0f);
      }
    }
  }
}

// ---- render cache: draw a region once, then copy the pixels on later frames

struct RCache { uint32_t key; int16_t w, h; LGFX_Sprite* s; uint32_t used; };
RCache rcache[72];
uint32_t rcTick = 0;
uint8_t artGen = 0;
uint8_t thumbGen[NTHUMB];

uint32_t hk(uint32_t h, uint32_t v) { return h ^ (v + 0x9e3779b9u + (h << 6) + (h >> 2)); }
uint32_t hs(uint32_t h, const char* s) { h ^= 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }

void cacheDraw(uint32_t key, int x, int y, int w, int h, std::function<void()> draw) {
  key = hk(hk(key, C_BG), C_TEXT);
  for (auto& e : rcache) {
    if (e.s && e.key == key && e.w == w && e.h == h) {
      e.s->pushSprite(&spr, x, y);
      e.used = ++rcTick;
      return;
    }
  }
  draw();
  int32_t cx, cy, cw, ch;
  spr.getClipRect(&cx, &cy, &cw, &ch);
  if (x < cx || y < cy || x + w > cx + cw || y + h > cy + ch) return;
  RCache* slot = &rcache[0];
  for (auto& e : rcache) {
    if (!e.s) { slot = &e; break; }
    if (e.used < slot->used) slot = &e;
  }
  if (slot->s && (slot->w != w || slot->h != h)) { slot->s->deleteSprite(); delete slot->s; slot->s = nullptr; }
  if (!slot->s) {
    slot->s = new LGFX_Sprite(&lcd);
    slot->s->setColorDepth(16);
    slot->s->setPsram(true);
    if (!slot->s->createSprite(w, h)) { delete slot->s; slot->s = nullptr; return; }
  }
  uint16_t* src = (uint16_t*)spr.getBuffer();
  uint16_t* dst = (uint16_t*)slot->s->getBuffer();
  for (int r = 0; r < h; r++) memcpy(dst + r * w, src + (y + r) * 320 + x, w * 2);
  slot->key = key;
  slot->w = w;
  slot->h = h;
  slot->used = ++rcTick;
}

int rssiCache = -127;
uint32_t rssiAt = 0;
int wifiLevel(bool up) {
  if (!up) return 0;
  if (!rssiAt || millis() - rssiAt > 2000) { rssiCache = WiFi.RSSI(); rssiAt = millis(); }
  return rssiCache > -60 ? 3 : rssiCache > -70 ? 2 : rssiCache > -80 ? 1 : 0;
}

// ---- icons

void icoLibrary(float x, float y, uint16_t c) {
  aaLine(x + 1.5f, y + 1, x + 1.5f, y + 11, 1.5f, c);
  aaLine(x + 5, y + 1, x + 5, y + 11, 1.5f, c);
  aaLine(x + 8, y + 1.2f, x + 10.3f, y + 11, 1.5f, c);
}

void icoBack(float cx, float cy, uint16_t c) {
  aaLine(cx + 2.4f, cy - 4.4f, cx - 2, cy, 1.6f, c);
  aaLine(cx - 2, cy, cx + 2.4f, cy + 4.4f, 1.6f, c);
}

void icoWifi(float cx, float cy, bool up, int level) {
  uint16_t on = C_TEXT, off = C_TRACK;
  aaCircle(cx, cy, 1.25f, up ? on : off);
  for (int i = 0; i < 3; i++) aaArc(cx, cy, 3.1f + i * 2.2f, 1.3f, 225, 315, up && level > i ? on : off);
}

void icoMoon(float cx, float cy, uint16_t c) {
  const float rA = 4.7f, bx = cx + 2.5f, by = cy - 2.6f, rB = 3.8f, w = 1.3f;
  aaPaint(cx - 7, cy - 7, cx + 7, cy + 7, c, [=](float x, float y) {
    float dA = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
    float dB = sqrtf((x - bx) * (x - bx) + (y - by) * (y - by));
    bool onA = fabsf(dA - rA) <= w / 2 && dB >= rB - w / 2;
    bool onB = fabsf(dB - rB) <= w / 2 && dA <= rA + w / 2;
    return onA || onB;
  });
}

void icoShuffle(float cx, float cy, uint16_t c) {
  const float w = 1.5f;
  aaLine(cx - 6, cy + 5.5f, cx + 4.6f, cy - 4.9f, w, c);
  aaLine(cx + 6, cy - 5.8f, cx + 1.8f, cy - 5.8f, w, c);
  aaLine(cx + 6, cy - 5.8f, cx + 6, cy - 1.6f, w, c);
  aaLine(cx - 6, cy - 5.5f, cx - 1.7f, cy - 1.2f, w, c);
  aaLine(cx + 1.2f, cy + 1.6f, cx + 4.6f, cy + 4.9f, w, c);
  aaLine(cx + 6, cy + 5.8f, cx + 1.8f, cy + 5.8f, w, c);
  aaLine(cx + 6, cy + 5.8f, cx + 6, cy + 1.6f, w, c);
}

void icoRepeat(float cx, float cy, uint16_t c, bool one) {
  const float w = 1.5f, r = 2.6f;
  aaLine(cx - 5.6f, cy - 0.3f, cx - 5.6f, cy - 4.4f + r, w, c);
  aaArc(cx - 5.6f + r, cy - 4.4f + r, r, w, 180, 270, c);
  aaLine(cx - 5.6f + r, cy - 4.4f, cx + 5.6f, cy - 4.4f, w, c);
  aaLine(cx + 5.8f, cy - 4.4f, cx + 3.2f, cy - 7, w, c);
  aaLine(cx + 5.8f, cy - 4.4f, cx + 3.2f, cy - 1.8f, w, c);
  aaLine(cx + 5.6f, cy + 0.3f, cx + 5.6f, cy + 4.4f - r, w, c);
  aaArc(cx + 5.6f - r, cy + 4.4f - r, r, w, 0, 90, c);
  aaLine(cx + 5.6f - r, cy + 4.4f, cx - 5.6f, cy + 4.4f, w, c);
  aaLine(cx - 5.8f, cy + 4.4f, cx - 3.2f, cy + 1.8f, w, c);
  aaLine(cx - 5.8f, cy + 4.4f, cx - 3.2f, cy + 7, w, c);
  if (one) {
    aaCircle(cx + 6.5f, cy - 6.5f, 5.2f, C_BG);
    aaCircle(cx + 6.5f, cy - 6.5f, 4.2f, c);
    txt("1", (int)(cx + 6.5f), (int)(cy - 6.5f), F_SB8, C_BG, textdatum_t::middle_center);
  }
}

void icoPrev(float cx, float cy, float s, uint16_t c) {
  float h = s / 2, bw = s * 0.17f;
  aaRRect(cx - s * 0.5f, cy - h, bw, s, bw / 2, c);
  aaTri(cx + s * 0.48f, cy - h, cx + s * 0.48f, cy + h, cx - s * 0.25f, cy, c);
}

void icoNext(float cx, float cy, float s, uint16_t c) {
  float h = s / 2, bw = s * 0.17f;
  aaRRect(cx + s * 0.5f - bw, cy - h, bw, s, bw / 2, c);
  aaTri(cx - s * 0.48f, cy - h, cx - s * 0.48f, cy + h, cx + s * 0.25f, cy, c);
}

void icoPlay(float cx, float cy, float s, uint16_t c) {
  aaTri(cx - s * 0.36f, cy - s * 0.5f, cx - s * 0.36f, cy + s * 0.5f, cx + s * 0.5f, cy, c);
}

void icoPause(float cx, float cy, float s, uint16_t c) {
  float bw = s * 0.3f;
  aaRRect(cx - s * 0.42f, cy - s / 2, bw, s, bw * 0.3f, c);
  aaRRect(cx + s * 0.42f - bw, cy - s / 2, bw, s, bw * 0.3f, c);
}

void icoHeart(float cx, float cy, float k, uint16_t c) {
  aaPaint(cx - k * 1.3f, cy - k * 1.4f, cx + k * 1.3f, cy + k * 1.2f, c, [=](float x, float y) {
    float u = (x - cx) / k, v = -(y - cy) / k + 0.15f;
    float a = u * u + v * v - 1;
    return a * a * a - u * u * v * v * v <= 0;
  });
}

void eqBars(float x, float base, uint16_t c, bool live) {
  float t = millis() / 160.0f;
  for (int b = 0; b < 3; b++) {
    float h = live ? 2.5f + 5.0f * (0.5f + 0.5f * sinf(t + b * 1.9f)) : (b == 1 ? 7.0f : b == 0 ? 4.0f : 5.5f);
    aaRRect(x + b * 2.8f, base - h, 1.8f, h, 0.5f, c);
  }
}

void iconSpeaker(int cx, int cy, int s, uint16_t c) {
  aaRRect(cx - s * 0.5f, cy - s * 0.2f, s * 0.3f, s * 0.4f, 1, c);
  aaTri(cx - s * 0.3f, cy - s * 0.2f, cx + s * 0.1f, cy - s * 0.5f, cx + s * 0.1f, cy + s * 0.5f, c);
  aaTri(cx - s * 0.3f, cy - s * 0.2f, cx - s * 0.3f, cy + s * 0.2f, cx + s * 0.1f, cy + s * 0.5f, c);
}

void dimRect(int x, int y, int w, int h, uint16_t to, float a) {
  for (int py = y; py < y + h; py++)
    for (int px = x; px < x + w; px++) blendPx(px, py, to, a);
}

const uint16_t PH_BG[5] = {RGB565(0x0E, 0x2A, 0x1B), RGB565(0x1F, 0x5F, 0x7A), RGB565(0xA3, 0x45, 0x2A), RGB565(0x2B, 0x4F, 0xA8), RGB565(0x6A, 0x4A, 0x26)};
const uint16_t C_LIKED = RGB565(0x4A, 0x3B, 0xD6);

void artPlaceholder(int x, int y, int s, uint16_t around) {
  spr.fillRect(x, y, s, s, PH_BG[0]);
  roundCorners(x, y, s, s, max(2, s / 23), around);
  float cx = x + s / 2.0f, cy = y + s / 2.0f;
  aaArc(cx, cy, s * 0.31f, max(0.9f, s * 0.013f), 0, 360, RGB565(0x17, 0x5E, 0x33));
  aaArc(cx, cy, s * 0.21f, max(1.1f, s * 0.019f), 0, 360, RGB565(0x18, 0x8A, 0x45));
  aaCircle(cx, cy, max(1.5f, s * 0.055f), C_GREEN);
}

void letterCover(const char* name, int x, int y, int s) {
  uint32_t h = 7;
  for (const char* p = name; *p; p++) h = h * 31 + (uint8_t)*p;
  int k = h % 5;
  spr.fillRect(x, y, s, s, PH_BG[k]);
  char l[2] = {'?', 0};
  for (const char* p = name; *p; p++) if (isalnum((unsigned char)*p)) { l[0] = toupper((unsigned char)*p); break; }
  txt(l, x + s / 2, y + s / 2 + 1, s >= 48 ? F_B22 : F_B16, k == 0 ? C_GREEN : RGB565(255, 255, 255), textdatum_t::middle_center);
}

const char* thumbUrlLocked(int slot);

void drawCover(int slot, uint8_t kind, const char* name, int x, int y, int s, int r, uint16_t around, uint8_t corners = 15) {
  uint32_t k = hk(hk(hk(hk(hs(11, name), kind), slot), s * 64 + r), around);
  k = hk(hk(k, corners), slot >= 0 ? thumbState[slot] * 256 + thumbGen[slot] : 0);
  cacheDraw(k, x, y, s, s, [=]() {
  if (kind == LK_LIKED) {
    spr.fillRect(x, y, s, s, C_LIKED);
    icoHeart(x + s / 2.0f, y + s / 2.0f, s * 0.125f, RGB565(255, 255, 255));
  } else if (slot >= 0 && thumbState[slot] == 4 && thumbs[slot]) {
    thumbs[slot]->pushRotateZoomWithAA(&spr, x + s / 2.0f - 0.5f, y + s / 2.0f - 0.5f, 0, (float)s / THUMB, (float)s / THUMB);
  } else {
    letterCover(name, x, y, s);
    if (slot >= 0 && thumbState[slot] == 0 && thumbUrlLocked(slot)[0]) thumbState[slot] = 1;
  }
  roundCorners(x, y, s, s, r, around, corners);
  });
}

// ---------------------------------------------------------------- UI state

Screen screen = SCR_BOOT;
Player snap;
uint32_t lastInteraction = 0, lastPlayingAt = 0;
bool wasPlaying = false;

uint32_t transStart = 0;
int transDir = 0;
bool transActive = false;

Scroller libScroll, listScroll;

TouchState tp;

char openListId[40] = "";
char openListName[64] = "";
char openListOwner[48] = "";
uint8_t openListKind = LK_PLAYLIST;
int openListIdx = -1;
int openListTotal = 0;
int openListSlot = -1;

uint8_t libTab = 0;
int tabX[2] = {12, 115}, tabW[2] = {97, 79};

int uiVol = 50;
float dispVol = 50;
uint32_t lastLocalVol = 0, lastStepMs = 0;
float volY = -50, volVY = 0;
uint32_t volShownAt = 0;
bool volVisible = false;


bool seeking = false;
float seekFrac = 0;

Overlay overlay = OV_NONE;
float ovPos = 0, ovTarget = 0;
uint32_t logoutArmedAt = 0;
LGFX_Sprite avatarSpr(&lcd);
bool avatarReady = false;

LGFX_Sprite ovSpr(&lcd);
uint32_t ovShownKey = 0;
LGFX_Sprite shownSpr(&lcd);
bool shownValid = false;
bool saverActive = false;
char lastTitle[96] = "", lastArtist[96] = "";
uint32_t saverStart = 0;
uint8_t edgeKind = 0;

void goScreen(Screen s, int dir) {
  if (s == screen) return;
  spr.pushSprite(&prevSpr, 0, 0);
  screen = s;
  uiScreen = s;
  if (s == SCR_NOW || s == SCR_LIBRARY) pollNow = true;
  transDir = dir;
  transStart = millis();
  transActive = dir != 0;
  Serial.printf("[ui] screen %d\n", (int)s);
}

void openList(const char* id, const char* name, int idx, int total, uint8_t kind, const char* owner, int slot) {
  LOCK();
  trGen = trGen + 1;
  trCount = 0;
  trLoading = true;
  trError = 0;
  trTotal = total;
  copyStr(trList, sizeof(trList), id);
  UNLOCK();
  copyStr(openListId, sizeof(openListId), id);
  copyStr(openListName, sizeof(openListName), name);
  copyStr(openListOwner, sizeof(openListOwner), owner);
  openListKind = kind;
  openListIdx = idx;
  openListTotal = total;
  openListSlot = slot;
  listScroll = Scroller();
  sendCmd(C_OPEN_LIST, 0, trGen, id);
  goScreen(SCR_LIST, 1);
}

void reopenList() {
  char id[40], name[64], owner[48];
  copyStr(id, sizeof(id), openListId);
  copyStr(name, sizeof(name), openListName);
  copyStr(owner, sizeof(owner), openListOwner);
  openList(id, name, openListIdx, openListTotal, openListKind, owner, openListSlot);
}

void openPlaylistRow(int k) {
  LOCK();
  bool ok = k >= 0 && k < plCount;
  char id[28], name[64], owner[48];
  int total = 0;
  if (ok) {
    copyStr(id, sizeof(id), pls[k].id);
    copyStr(name, sizeof(name), pls[k].name);
    copyStr(owner, sizeof(owner), pls[k].sub);
    total = pls[k].total;
  }
  UNLOCK();
  if (ok) openList(id, name, k, total, LK_PLAYLIST, owner, k);
}

void openRecent(int i) {
  LOCK();
  bool ok = i >= 0 && i < recentCount;
  RecentItem r;
  if (ok) r = recent[i];
  UNLOCK();
  if (!ok) return;
  if (r.kind == LK_LIKED) openList("liked", "Liked Songs", -1, 0, LK_LIKED, "You", -1);
  else openList(r.key, r.name, r.plIdx, r.total, r.kind, r.owner, r.plIdx >= 0 ? r.plIdx : MAX_PL + i);
}

void openLibrary(int dir) {
  goScreen(SCR_LIBRARY, dir);
  if (!plLoadedOnce && !plLoading) sendCmd(C_RELOAD_PL);
}

uint32_t curProgress() {
  if (!snap.active) return 0;
  uint32_t p = snap.progress + (snap.playing ? millis() - snap.fetchedAt : 0);
  return snap.duration ? min(p, snap.duration) : p;
}

void contextLabel(char* out, size_t n) {
  const char* c = snap.context;
  out[0] = 0;
  if (strncmp(c, "spotify:playlist:", 17) == 0) {
    const char* id = c + 17;
    LOCK();
    for (int i = 0; i < plCount && !out[0]; i++) if (strcmp(pls[i].id, id) == 0) copyStr(out, n, pls[i].name);
    for (int i = 0; i < recentCount && !out[0]; i++) if (strcmp(recent[i].key, id) == 0) copyStr(out, n, recent[i].name);
    UNLOCK();
    if (!out[0]) copyStr(out, n, "Playlist");
  } else if (strstr(c, ":collection")) {
    copyStr(out, n, "Liked Songs");
  } else {
    copyStr(out, n, snap.album[0] ? snap.album : snap.isTrack ? "Now playing" : "Podcast");
  }
}

// ---------------------------------------------------------------- actions

void touchActivity() {
  lastInteraction = millis();
}

void actTogglePlay() {
  bool play;
  LOCK();
  play = !shared.playing;
  shared.progress = curProgress();
  shared.fetchedAt = millis();
  shared.playing = play;
  holdPlayUntil = millis() + 2500;
  UNLOCK();
  sendCmd(C_TOGGLE, play);
}

void actSkip(int dir) {
  LOCK();
  shared.progress = 0;
  shared.fetchedAt = millis();
  holdSeekUntil = 0;
  copyStr(skipFromUri, sizeof(skipFromUri), shared.uri);
  wantUri[0] = 0;
  holdTrackUntil = millis() + 3000;
  UNLOCK();
  sendCmd(dir > 0 ? C_NEXT : C_PREV);
}

void actShuffle() {
  bool s;
  LOCK();
  s = !shared.shuffle;
  shared.shuffle = s;
  holdShuffleUntil = millis() + 3000;
  UNLOCK();
  sendCmd(C_SHUFFLE, s);
}

void actRepeat() {
  uint8_t r;
  LOCK();
  r = (shared.repeat + 1) % 3;
  shared.repeat = r;
  holdRepeatUntil = millis() + 3000;
  UNLOCK();
  sendCmd(C_REPEAT, r);
}

void actSeek(float frac) {
  uint32_t pos = (uint32_t)(constrain(frac, 0.0f, 1.0f) * snap.duration);
  LOCK();
  shared.progress = pos;
  shared.fetchedAt = millis();
  holdSeekUntil = millis() + 2500;
  UNLOCK();
  sendCmd(C_SEEK, pos);
}

void actPrev() {
  if (curProgress() > 3000) actSeek(0);
  else actSkip(-1);
}

void actPlayAt(int index, bool shuffle) {
  sendCmd(C_PLAY_AT, index, shuffle, openListId);
  LOCK();
  shared.playing = true;
  holdPlayUntil = millis() + 2500;
  if (!shuffle && index >= 0 && index < trCount) {
    copyStr(wantUri, sizeof(wantUri), trs[index].uri);
    skipFromUri[0] = 0;
    holdTrackUntil = millis() + 3500;
    copyStr(shared.title, sizeof(shared.title), trs[index].name);
    copyStr(shared.artist, sizeof(shared.artist), trs[index].artist);
    copyStr(shared.uri, sizeof(shared.uri), trs[index].uri);
    shared.progress = 0;
    shared.fetchedAt = millis();
    shared.duration = trs[index].dur;
    shared.active = true;
  }
  if (openListKind == LK_LIKED) copyStr(shared.context, sizeof(shared.context), "spotify:user:me:collection");
  else if (openListKind == LK_ALBUM) snprintf(shared.context, sizeof(shared.context), "spotify:album:%s", openListId + 6);
  else snprintf(shared.context, sizeof(shared.context), "spotify:playlist:%s", openListId);
  UNLOCK();
  marqueeStart = millis();
  goScreen(SCR_NOW, 1);
}

void bumpVolume(int dir) {
  uint32_t now = millis();
  uint32_t gap = now - lastStepMs;
  lastStepMs = now;
  int step = gap < 25 ? 5 : gap < 50 ? 3 : gap < 90 ? 2 : 1;
  int nv = constrain(uiVol + dir * step, 0, 100);
  if (nv == uiVol) {
    volShownAt = now;
    volVisible = true;
    return;
  }
  uiVol = nv;
  lastLocalVol = now;
  volShownAt = now;
  volVisible = true;
  LOCK();
  shared.volume = nv;
  holdVolUntil = now + 2500;
  UNLOCK();
  volTarget = nv;
  volChangedAt = now;
  volDirty = true;
  Serial.printf("[enc] %s -> vol %d (step %d)\n", dir > 0 ? "up" : "down", nv, step);
}

// ---------------------------------------------------------------- screens

void drawToast() {
  char m[96];
  uint32_t at;
  LOCK();
  copyStr(m, sizeof(m), toastMsg);
  at = toastAt;
  UNLOCK();
  if (!m[0]) return;
  uint32_t age = millis() - at;
  if (age > 2800) return;
  float a = age < 200 ? age / 200.0f : age > 2500 ? 1 - (age - 2500) / 300.0f : 1;
  int w = min(300, textW(m, F_SB11) + 28);
  int y = 240 - (int)(38 * easeOutCubic(a));
  spr.fillSmoothRoundRect(160 - w / 2, y, w, 28, 14, mix(C_TEXT, C_BG, a));
  txt(m, 160, y + 14, F_SB11, C_ON, textdatum_t::middle_center, w - 20);
}

void drawBoot() {
  spr.fillScreen(C_BG);
  float t = millis() / 1000.0f;
  for (int i = 0; i < 8; i++) {
    float a = t * 4 + i * 0.785f;
    aaCircle(160 + cosf(a) * 16, 96 + sinf(a) * 16, 2.6f, mix(C_GREEN, C_BG, 1 - i / 8.0f));
  }
  txt(joining ? "Joining Wi-Fi" : "Connecting to Wi-Fi", 160, 140, F_SB12, C_TEXT, textdatum_t::middle_center);
  txt(joining ? joinSsid : wifiSsid, 160, 158, F_R11, C_SUB, textdatum_t::middle_center, 280);
  if (!joining && millis() - bootStart > 4000) txt("Tap to choose a different network", 160, 214, F_R10, C_DIM, textdatum_t::middle_center);
}

void drawSetup() {
  spr.fillScreen(C_BG);
  String url = "http://" + WiFi.localIP().toString();
  spr.fillSmoothRoundRect(14, 40, 128, 128, 8, RGB565(255, 255, 255));
  spr.qrcode(url.c_str(), 22, 48, 112, 2);
  txt("Connect Spotify", 160, 18, F_B16, C_TEXT, textdatum_t::middle_center);
  txt("1. Scan or open", 156, 56, F_R12, C_SUB);
  txt(url.c_str(), 156, 74, F_SB12, C_GREEN, textdatum_t::top_left, 160);
  txt("2. Follow the 3 steps", 156, 102, F_R12, C_SUB);
  txt("on that page", 156, 118, F_R12, C_SUB);
  txt("3. Music shows up here", 156, 144, F_R12, C_SUB);
  txt(DEVICE_HOSTNAME ".local also works", 160, 196, F_R10, C_DIM, textdatum_t::top_center);
}

void drawIdle() {
  spr.fillScreen(C_BG);
}

void drawTopBar(uint8_t left) {
  if (left == 0) cacheDraw(101, 11, 5, 16, 16, []() { icoLibrary(13, 7, C_SUB2); });
  else if (left == 1) txt("Library", 12, 13, F_SB11, C_SUB, textdatum_t::middle_left);
  else cacheDraw(102, 12, 6, 14, 15, []() { icoBack(19, 13, C_TEXT); });
  uint32_t age = lastGoodPoll ? millis() - lastGoodPoll : 0;
  char b[12];
  if (loggedIn() && lastGoodPoll && age > 20000) txt("Reconnecting" ELLIPSIS, 160, 13, F_SB11, C_ORANGE, textdatum_t::middle_center);
  else if (fmtClock(b, sizeof(b))) txt(b, 160, 13, F_SB12, C_TEXT, textdatum_t::middle_center);
  bool up = WiFi.status() == WL_CONNECTED;
  int lv = wifiLevel(up);
  cacheDraw(hk(103, up * 8 + lv), 268, 5, 20, 15, [=]() { icoWifi(278, 16.2f, up, lv); });
  cacheDraw(104, 293, 5, 16, 16, []() { icoMoon(301, 13, C_SUB2); });
}

// ---- Now Playing

const int NP_ART_X = 16, NP_ART_Y = 42, NP_TX = 122, NP_TW = 186;
const int PB_X = 16, PB_W = 288, PB_Y = 149;
const int CTRL_Y = 194;
const int CX_SHUF = 37, CX_PREV = 97, CX_PLAY = 160, CX_NEXT = 223, CX_REP = 284;

void drawNow() {
  spr.fillScreen(C_BG);
  drawTopBar(0);
  bool has = snap.active && snap.title[0];
  bool art = has && artShownUrl[0];
  cacheDraw(hk(200, art ? 1000 + artGen : 0), NP_ART_X, NP_ART_Y, ART, ART, [=]() {
    if (art) {
      artSpr.pushSprite(&spr, NP_ART_X, NP_ART_Y);
      roundCorners(NP_ART_X, NP_ART_Y, ART, ART, 4, C_BG);
    } else {
      artPlaceholder(NP_ART_X, NP_ART_Y, ART, C_BG);
    }
  });

  if (has) {
    char ctx[64];
    contextLabel(ctx, sizeof(ctx));
    capsTxt(ctx, NP_TX, 66, F_SB9, C_SUB, 1.9f, NP_TW);
    txt(snap.title, NP_TX, 88, F_B16, C_TEXT, textdatum_t::middle_left, NP_TW);
    txt(snap.artist, NP_TX, 107, F_R12, C_SUB, textdatum_t::middle_left, NP_TW);
  } else {
    capsTxt("Not playing", NP_TX, 66, F_SB9, C_SUB, 1.9f, NP_TW);
    txt("Nothing playing", NP_TX, 88, F_B16, C_TEXT, textdatum_t::middle_left, NP_TW);
    txt("Play on any device, or", NP_TX, 107, F_R12, C_SUB, textdatum_t::middle_left, NP_TW);
    txt("tap here for your Library", NP_TX, 122, F_R12, C_SUB, textdatum_t::middle_left, NP_TW);
  }

  uint32_t pos = curProgress();
  float frac = snap.duration ? (float)pos / snap.duration : 0;
  if (seeking) frac = seekFrac;
  spr.fillSmoothRoundRect(PB_X, PB_Y - 1, PB_W, 3, 1, C_TRACK);
  int fx = PB_X + (int)(PB_W * frac);
  if (fx > PB_X + 1) spr.fillSmoothRoundRect(PB_X, PB_Y - 1, fx - PB_X, 3, 1, C_GREEN);
  if (has) aaCircle(fx + 0.5f, PB_Y + 0.5f, seeking ? 6.0f : 4.0f, C_TEXT);
  char a[12], b[12];
  fmtTime(a, seeking ? (uint32_t)(seekFrac * snap.duration) : pos);
  fmtTime(b, snap.duration);
  txt(a, PB_X + 1, 161, F_R9, C_SUB, textdatum_t::middle_left);
  txt(b, PB_X + PB_W, 161, F_R9, C_SUB, textdatum_t::middle_right);

  uint16_t ink = has ? C_TEXT : C_DIM;
  uint16_t shufC = snap.shuffle ? C_GREEN : ink, repC = snap.repeat ? C_GREEN : ink;
  bool one = snap.repeat == 2, playing = snap.playing;
  bool pressPlay = tp.down && !tp.moved && abs(tp.sx - CX_PLAY) < 28 && abs(tp.sy - CTRL_Y) < 26;
  cacheDraw(hk(210, shufC), CX_SHUF - 9, CTRL_Y - 9, 20, 20, [=]() { icoShuffle(CX_SHUF + 0.5f, CTRL_Y + 0.5f, shufC); });
  if (snap.shuffle) aaCircle(CX_SHUF + 0.5f, 206.5f, 1.4f, C_GREEN);
  cacheDraw(hk(211, ink), CX_PREV - 8, CTRL_Y - 8, 18, 18, [=]() { icoPrev(CX_PREV + 0.5f, CTRL_Y + 0.5f, 10, ink); });
  cacheDraw(hk(212, has * 4 + playing * 2 + pressPlay), CX_PLAY - 24, CTRL_Y - 24, 50, 50, [=]() {
    aaCircle(CX_PLAY + 0.5f, CTRL_Y + 0.5f, pressPlay ? 21.5f : 23, has ? C_TEXT : C_CARD2);
    if (playing) icoPause(CX_PLAY + 0.5f, CTRL_Y + 0.5f, 11, C_ON);
    else icoPlay(CX_PLAY + 1.5f, CTRL_Y + 0.5f, 12, has ? C_ON : C_DIM);
  });
  cacheDraw(hk(213, ink), CX_NEXT - 8, CTRL_Y - 8, 18, 18, [=]() { icoNext(CX_NEXT + 0.5f, CTRL_Y + 0.5f, 10, ink); });
  cacheDraw(hk(214, repC * 2 + one), CX_REP - 9, CTRL_Y - 13, 23, 23, [=]() { icoRepeat(CX_REP + 0.5f, CTRL_Y + 0.5f, repC, one); });
  if (snap.repeat) aaCircle(CX_REP + 0.5f, 206.5f, 1.4f, C_GREEN);
}

// ---- Library (Recently played / Your Library)

const int TAB_Y = 28, TAB_H = 22, LIB_TOP = 58, LIB_ROW = 46;
const int MINI_Y = 196;
const int RT_X[2] = {12, 163};
const int RT_W = 145, RT_H = 40, RT_ROW = 46;

void drawMiniPlayer() {
  if (!snap.active || !snap.title[0]) return;
  spr.fillSmoothRoundRect(6, MINI_Y, 308, 38, 4, C_MINI);
  bool art = artShownUrl[0];
  cacheDraw(hk(220, art ? 1000 + artGen : 0), 10, 200, 30, 30, [=]() {
    if (art) {
      artSpr.pushRotateZoomWithAA(&spr, 24.5f, 214.5f, 0, 30.0f / ART, 30.0f / ART);
      roundCorners(10, 200, 30, 30, 2, C_MINI);
    } else {
      artPlaceholder(10, 200, 30, C_MINI);
    }
  });
  txt(snap.title, 48, 209, F_SB12, C_TEXT, textdatum_t::middle_left, 194);
  txt(snap.artist, 48, 222, F_R10, C_SUB, textdatum_t::middle_left, 194);
  bool playing = snap.playing;
  cacheDraw(hk(221, playing), 257, 207, 16, 16, [=]() {
    if (playing) icoPause(264.5f, 214.5f, 9, C_TEXT);
    else icoPlay(265.5f, 214.5f, 10, C_TEXT);
  });
  cacheDraw(222, 289, 208, 15, 14, []() { icoNext(296, 214.5f, 8, C_TEXT); });
  float frac = snap.duration ? (float)curProgress() / snap.duration : 0;
  spr.fillRect(10, 231, 300, 2, C_TRACK);
  spr.fillRect(10, 231, (int)(300 * constrain(frac, 0.0f, 1.0f)), 2, C_GREEN);
}

void drawTabs() {
  cacheDraw(hk(230, libTab), 10, TAB_Y - 1, 230, TAB_H + 2, []() {
    const char* names[2] = {"Recently played", "Your Library"};
    int x = 12;
    for (int i = 0; i < 2; i++) {
      int w = textW(names[i], F_SB11) + 20;
      tabX[i] = x;
      tabW[i] = w;
      bool on = libTab == i;
      spr.fillSmoothRoundRect(x, TAB_Y, w, TAB_H, TAB_H / 2, on ? C_GREEN : C_CARD2);
      txt(names[i], x + w / 2, TAB_Y + TAB_H / 2, F_SB11, on ? RGB565(0, 0, 0) : C_TEXT, textdatum_t::middle_center);
      x += w + 6;
    }
  });
}

void drawRecent(int bottom) {
  LOCK();
  int n = recentCount;
  uint8_t st = recentState;
  RecentItem items[6];
  for (int i = 0; i < n; i++) items[i] = recent[i];
  UNLOCK();
  if (n == 0) {
    if (st == RS_SCOPE) {
      txt("Recently played needs one more permission.", 160, 104, F_R11, C_SUB, textdatum_t::middle_center, 296);
      txt("Log out and log back in to allow it.", 160, 122, F_R11, C_SUB, textdatum_t::middle_center, 296);
    } else if (st == RS_ERROR) {
      txt("Couldn't load recently played. Tap to retry.", 160, 112, F_R11, C_SUB, textdatum_t::middle_center, 296);
    } else if (st == RS_OK) {
      txt("Nothing played recently", 160, 112, F_R11, C_SUB, textdatum_t::middle_center);
    } else {
      txt("Loading" ELLIPSIS, 160, 112, F_R11, C_SUB, textdatum_t::middle_center);
    }
    return;
  }
  for (int i = 0; i < n; i++) {
    int x = RT_X[i % 2], y = LIB_TOP + (i / 2) * RT_ROW;
    if (y + RT_H > bottom) break;
    bool pressed = tp.down && !tp.moved && tp.sx >= x && tp.sx < x + RT_W && tp.sy >= y && tp.sy < y + RT_H;
    int slot = items[i].kind == LK_LIKED ? -1 : items[i].plIdx >= 0 ? items[i].plIdx : MAX_PL + i;
    RecentItem it = items[i];
    uint32_t k = hk(hk(hk(hs(240, it.name), pressed), slot), slot >= 0 ? thumbState[slot] * 256 + thumbGen[slot] : 0);
    cacheDraw(hk(k, it.kind), x, y, RT_W, RT_H, [=]() {
      spr.fillSmoothRoundRect(x, y, RT_W, RT_H, 3, pressed ? C_CARD2 : C_CARD);
      drawCover(slot, it.kind, it.name, x, y, RT_H, 3, C_BG, 1 | 4);
      txt(it.name, x + 48, y + RT_H / 2, F_SB11, C_TEXT, textdatum_t::middle_left, RT_W - 54);
    });
  }
}

void drawLibList(int bottom) {
  int n = 1 + plCount;
  libScroll.maxY = max(0.0f, (float)(n * LIB_ROW + 4 - (bottom - LIB_TOP)));
  spr.setClipRect(0, LIB_TOP, 320, bottom - LIB_TOP);
  const char* ctx = snap.context;
  bool likedPlaying = strstr(ctx, ":collection") != nullptr;
  const char* playingPl = strncmp(ctx, "spotify:playlist:", 17) == 0 ? ctx + 17 : "";
  int first = max(0, (int)(libScroll.y / LIB_ROW));
  LOCK();
  for (int i = first; i < n; i++) {
    int y = LIB_TOP + i * LIB_ROW - (int)libScroll.y;
    if (y > bottom) break;
    bool pressed = tp.down && !tp.moved && tp.sy >= y && tp.sy < y + LIB_ROW && tp.sy >= LIB_TOP;
    bool playing = i == 0 ? likedPlaying && snap.active : snap.active && playingPl[0] && strcmp(playingPl, pls[i - 1].id) == 0;
    const char* nm = i == 0 ? "Liked Songs" : pls[i - 1].name;
    const char* ow = i == 0 ? "You" : pls[i - 1].sub;
    uint32_t k = hk(hk(hk(hs(hs(250, nm), ow), pressed * 2 + playing), i), i > 0 ? thumbState[i - 1] * 256 + thumbGen[i - 1] : 0);
    cacheDraw(k, 4, y, 312, LIB_ROW, [=]() {
      if (pressed) spr.fillSmoothRoundRect(4, y, 312, LIB_ROW, 4, C_MINI);
      if (i == 0) drawCover(-1, LK_LIKED, "Liked Songs", 12, y + 3, 40, 3, pressed ? C_MINI : C_BG);
      else drawCover(i - 1, LK_PLAYLIST, nm, 12, y + 3, 40, 3, pressed ? C_MINI : C_BG);
      txt(nm, 62, y + 16, F_SB12, playing ? C_GREEN : C_TEXT, textdatum_t::middle_left, 220);
      char sub[80];
      snprintf(sub, sizeof(sub), "Playlist " MIDDOT " %s", ow);
      txt(sub, 62, y + 31, F_R10, C_SUB, textdatum_t::middle_left, 220);
    });
    if (playing) eqBars(294, y + 27, C_GREEN, snap.playing);
  }
  int count = plCount;
  UNLOCK();
  spr.clearClipRect();
  if (plLoading && count == 0) txt("Loading" ELLIPSIS, 160, LIB_TOP + LIB_ROW + 20, F_R11, C_SUB, textdatum_t::middle_center);
}

void drawLibrary() {
  spr.fillScreen(C_BG);
  bool mini = snap.active && snap.title[0];
  int bottom = mini ? MINI_Y - 4 : 240;
  if (libTab == 0) {
    if (loggedIn() && recentState != RS_LOADING && !needRecent && (!recentAt || millis() - recentAt > 60000) && recentState != RS_SCOPE) needRecent = true;
    drawRecent(bottom);
  } else {
    drawLibList(bottom);
  }
  drawTopBar(1);
  drawTabs();
  drawMiniPlayer();
}

// ---- Playlist tracks

const int TR_TOP = 88, TR_ROW = 36;

void drawList() {
  spr.fillScreen(C_BG);
  spr.setClipRect(0, TR_TOP, 320, 240 - TR_TOP);
  LOCK();
  int n = trCount;
  int total = max((int)trTotal, n);
  listScroll.maxY = max(0.0f, (float)(n * TR_ROW + 44 - (240 - TR_TOP)));
  int sy = (int)listScroll.y;
  int first = max(0, sy / TR_ROW);
  char dur[12], num[8];
  for (int i = first; i < n; i++) {
    int y = TR_TOP + i * TR_ROW - sy;
    if (y > 240) break;
    bool cur = trs[i].uri[0] && strcmp(trs[i].uri, snap.uri) == 0;
    bool dead = !trs[i].uri[0];
    bool pressed = tp.down && !tp.moved && tp.target == 2 && tp.sy >= TR_TOP && tp.sy + sy >= TR_TOP + i * TR_ROW && tp.sy + sy < TR_TOP + (i + 1) * TR_ROW;
    TR t = trs[i];
    uint32_t k = hk(hk(hs(hs(260, t.name), t.artist), i), t.dur * 8 + cur * 4 + pressed * 2 + dead);
    cacheDraw(k, 6, y, 308, TR_ROW, [=, &num, &dur]() {
      if (cur || pressed) spr.fillSmoothRoundRect(6, y, 308, TR_ROW, 3, pressed ? C_CARD : C_MINI);
      if (!cur) {
        snprintf(num, sizeof(num), "%d", i + 1);
        txt(num, 21, y + 18, F_R10, C_SUB, textdatum_t::middle_center);
      }
      txt(t.name, 38, y + 12, F_R12, dead ? C_DIM : cur ? C_GREEN : C_TEXT, textdatum_t::middle_left, 232);
      txt(t.artist, 38, y + 25, F_R10, C_SUB, textdatum_t::middle_left, 232);
      if (t.dur) {
        fmtTime(dur, t.dur);
        txt(dur, 305, y + 18, F_R10, C_SUB, textdatum_t::middle_right);
      }
    });
    if (cur) eqBars(17, y + 22, C_GREEN, snap.playing);
  }
  int ey = TR_TOP + n * TR_ROW - sy + 18;
  if (trLoading) {
    txt("Loading" ELLIPSIS, 160, ey, F_R11, C_SUB, textdatum_t::middle_center);
  } else if (trError && n == 0) {
    const char* a = "Couldn't load these songs";
    char b[64];
    if (trError == 403) { a = "Spotify only shares songs from"; snprintf(b, sizeof(b), "playlists you made or collaborate on"); }
    else if (trError == 429) snprintf(b, sizeof(b), "Spotify is busy. Tap to try again.");
    else snprintf(b, sizeof(b), "Error %d. Tap to try again.", trError);
    txt(a, 160, ey, F_R11, C_SUB, textdatum_t::middle_center, 296);
    txt(b, 160, ey + 16, F_R10, C_DIM, textdatum_t::middle_center, 296);
  } else if (n == 0) {
    txt("Nothing here yet", 160, ey, F_R11, C_SUB, textdatum_t::middle_center);
  }
  bool needMore = !trLoading && !trError && n > 0 && n < trTotal && n < MAX_TR && listScroll.y > listScroll.maxY - 300;
  UNLOCK();
  spr.clearClipRect();
  if (needMore) {
    trLoading = true;
    sendCmd(C_MORE, n, trGen, openListId);
  }

  drawTopBar(2);
  drawCover(openListSlot, openListKind, openListName, 12, 30, 52, 4, C_BG);
  capsTxt(openListKind == LK_ALBUM ? "Album" : "Playlist", 74, 40, F_SB8, C_SUB, 1.8f, 150);
  txt(openListName, 74, 55, F_B15, C_TEXT, textdatum_t::middle_left, 156);
  char sub[96];
  if (openListOwner[0]) snprintf(sub, sizeof(sub), "%s " MIDDOT " %d song%s", openListOwner, total, total == 1 ? "" : "s");
  else snprintf(sub, sizeof(sub), "%d song%s", total, total == 1 ? "" : "s");
  txt(sub, 74, 71, F_R10, C_SUB, textdatum_t::middle_left, 156);
  cacheDraw(270, 236, 47, 20, 20, []() { icoShuffle(245.5f, 56.5f, C_GREEN); });
  bool pressPlay = tp.down && !tp.moved && hypotf(tp.sx - 289, tp.sy - 56) < 22;
  cacheDraw(hk(271, pressPlay), 269, 36, 42, 42, [=]() {
    aaCircle(289.5f, 56.5f, pressPlay ? 17.5f : 19, C_GREEN);
    icoPlay(290.5f, 56.5f, 10, RGB565(0, 0, 0));
  });
}

// ---- volume

void drawVolumeOverlay(float dt) {
  uint32_t now = millis();
  if (volVisible && now - volShownAt > 1600) volVisible = false;
  float target = volVisible ? 8 : -52;
  float k = 260, damp = 18;
  volVY += ((target - volY) * k - volVY * damp) * dt;
  volY += volVY * dt;
  if (!volVisible && volY < -50) { volY = -52; volVY = 0; return; }
  dispVol += (uiVol - dispVol) * min(1.0f, dt * 14);

  int y = (int)volY, x = 40, w = 240, h = 34;
  uint16_t bar = uiVol <= 60 ? C_GREEN : uiVol <= 80 ? C_ORANGE : C_RED;
  spr.fillSmoothRoundRect(x, y, w, h, 17, RGB565(0x1A, 0x1A, 0x1A));
  iconSpeaker(x + 20, y + h / 2, 14, RGB565(255, 255, 255));
  if (uiVol == 0) {
    aaLine(x + 26, y + 12, x + 33, y + 22, 1.4f, C_RED);
    aaLine(x + 26, y + 22, x + 33, y + 12, 1.4f, C_RED);
  } else {
    for (int i = 0; i < 2; i++) if (uiVol > i * 50) aaArc(x + 22, y + h / 2.0f, 5.0f + i * 3.6f, 1.3f, -50, 50, RGB565(255, 255, 255));
  }
  int bx = x + 44, bw = 142, by = y + h / 2 - 2;
  spr.fillSmoothRoundRect(bx, by, bw, 4, 2, C_TRACK);
  int fw = (int)(bw * dispVol / 100.0f);
  if (fw > 2) spr.fillSmoothRoundRect(bx, by, fw, 4, 2, bar);
  aaCircle(bx + fw + 0.5f, by + 2, 5, RGB565(255, 255, 255));
  char p[8];
  snprintf(p, sizeof(p), "%d%%", (int)(dispVol + 0.5f));
  txt(p, x + w - 14, y + h / 2, F_SB12, bar, textdatum_t::middle_right);
}

// ---------------------------------------------------------------- wi-fi

WNet nets[24];
int netCount = 0;
bool scanning = false;
uint32_t scanAt = 0;
Scroller wifiScroll;
char wifiMsg[80] = "";
bool wifiAuto = false;
Screen wifiReturn = SCR_BOOT;
uint8_t kbLayer = 0;
bool kbShow = false;
int kbFlash = -1;
uint32_t kbFlashAt = 0;
Key keys[40];
int keyCount = 0;

const int WIFI_TOP = 44, WIFI_ROW = 44;
const int KB_Y = 80, KB_H = 40;
const int PW_Y = 34, PW_H = 40;

const char* const KB_ROWS[4][3] = {
  {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
  {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
  {"1234567890", "-/:;()$&@", ".,?!'\"_"},
  {"[]{}#%^*+=", "\\|~<>`;:@", ".,?!'\"-"},
};

void resumeSaved() {
  if (WiFi.status() == WL_CONNECTED || joining || !wifiSsid[0]) return;
  WiFi.begin(wifiSsid, wifiPass);
}

void startScan() {
  if (scanning) return;
  if (WiFi.status() != WL_CONNECTED && !joining) WiFi.disconnect();
  WiFi.scanDelete();
  scanning = WiFi.scanNetworks(true, false) != WIFI_SCAN_FAILED;
  scanAt = millis();
  Serial.println("[wifi] scanning");
}

void pollScan() {
  if (!scanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING && millis() - scanAt < 15000) return;
  scanning = false;
  scanAt = millis();
  if (n > 0) {
    netCount = 0;
    for (int i = 0; i < n; i++) {
      String id = WiFi.SSID(i);
      if (!id.length()) continue;
      int8_t r = WiFi.RSSI(i);
      int k = 0;
      while (k < netCount && id != nets[k].ssid) k++;
      if (k < netCount) { if (r > nets[k].rssi) nets[k].rssi = r; continue; }
      if (netCount >= 24) continue;
      copyStr(nets[netCount].ssid, sizeof(nets[0].ssid), id.c_str());
      nets[netCount].rssi = r;
      nets[netCount].open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
      netCount++;
    }
    for (int i = 1; i < netCount; i++) {
      WNet t = nets[i];
      int j = i - 1;
      while (j >= 0 && nets[j].rssi < t.rssi) { nets[j + 1] = nets[j]; j--; }
      nets[j + 1] = t;
    }
    Serial.printf("[wifi] found %d networks\n", netCount);
  }
  WiFi.scanDelete();
  resumeSaved();
}

void openWifi(bool automatic) {
  wifiAuto = automatic;
  if (automatic) wifiReturn = SCR_BOOT;
  else {
    wifiMsg[0] = 0;
    if (screen != SCR_WIFI && screen != SCR_WIFIPASS) wifiReturn = screen;
  }
  wifiScroll.y = 0;
  wifiScroll.vel = 0;
  goScreen(SCR_WIFI, automatic ? 0 : 1);
  startScan();
}

void beginJoin() {
  if (scanning) { esp_wifi_scan_stop(); WiFi.scanDelete(); scanning = false; }
  WiFi.disconnect();
  delay(100);
  WiFi.begin(joinSsid, joinOpen ? nullptr : joinPass);
  joining = true;
  joinStart = millis();
  wifiMsg[0] = 0;
  Serial.printf("[wifi] joining %s\n", joinSsid);
  goScreen(SCR_BOOT, 0);
}

void pickNetwork(int i) {
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == nets[i].ssid) { setToast("Already connected"); return; }
  copyStr(joinSsid, sizeof(joinSsid), nets[i].ssid);
  joinOpen = nets[i].open;
  if (joinOpen) { beginJoin(); return; }
  copyStr(joinPass, sizeof(joinPass), strcmp(joinSsid, wifiSsid) == 0 ? wifiPass : "");
  kbLayer = 0;
  kbShow = false;
  wifiMsg[0] = 0;
  goScreen(SCR_WIFIPASS, 1);
}

void wifiBars(int x, int cy, int rssi, uint16_t on, uint16_t off) {
  int lv = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 4;
    spr.fillSmoothRoundRect(x + i * 6, cy + 8 - h, 4, h, 1, i < lv ? on : off);
  }
}

void iconLock(int cx, int cy, uint16_t c) {
  spr.drawCircle(cx, cy - 3, 4, c);
  spr.drawCircle(cx, cy - 3, 3, c);
  spr.fillSmoothRoundRect(cx - 6, cy - 1, 12, 9, 2, c);
}

void drawWifi() {
  spr.fillScreen(C_BG);
  bool up = WiFi.status() == WL_CONNECTED;
  String cur = up ? WiFi.SSID() : String("");
  wifiScroll.maxY = max(0.0f, (float)(WIFI_TOP + netCount * WIFI_ROW + 8 - 240));
  int sy = (int)wifiScroll.y;
  spr.setClipRect(0, WIFI_TOP, 320, 240 - WIFI_TOP);
  for (int i = 0; i < netCount; i++) {
    int y = WIFI_TOP + i * WIFI_ROW - sy;
    if (y < WIFI_TOP - WIFI_ROW || y > 240) continue;
    bool isCur = up && cur == nets[i].ssid;
    int cy = y + WIFI_ROW / 2;
    spr.fillSmoothRoundRect(10, y + 3, 300, WIFI_ROW - 6, 10, C_CARD);
    wifiBars(22, cy, nets[i].rssi, isCur ? C_GREEN : C_TEXT, C_CARD2);
    txt(nets[i].ssid, 54, cy + 1, F_SMB, isCur ? C_GREEN : C_TEXT, textdatum_t::middle_left, isCur ? 170 : 210);
    if (isCur) txt("Connected", 298, cy + 1, F_TINY, C_GREEN, textdatum_t::middle_right);
    else if (!nets[i].open) iconLock(290, cy, C_SUB);
  }
  if (!netCount) txt(scanning ? "Looking for networks..." : "No networks found. Tap Scan.", 160, 140, F_SMALL, C_SUB, textdatum_t::middle_center);
  spr.clearClipRect();

  txt("Wi-Fi", 14, 6, F_MED, C_TEXT);
  char sub[96];
  if (wifiMsg[0]) snprintf(sub, sizeof(sub), "%s", wifiMsg);
  else if (up) snprintf(sub, sizeof(sub), "Connected to %s", cur.c_str());
  else snprintf(sub, sizeof(sub), "Pick a network to join");
  txt(sub, 14, 28, F_TINY, wifiMsg[0] ? C_RED : C_SUB, textdatum_t::top_left, 220);

  spr.fillSmoothRoundRect(246, 8, 62, 28, 14, C_CARD);
  if (scanning) {
    float t = millis() / 1000.0f;
    for (int i = 0; i < 3; i++) spr.fillCircle(265 + i * 12, 22, 3, mix(C_GREEN, C_CARD, 0.5f + 0.5f * sinf(t * 6 - i)));
  } else {
    txt("Scan", 277, 23, F_SMB, C_GREEN, textdatum_t::middle_center);
  }
}

void buildKeys() {
  keyCount = 0;
  auto add = [](int x, int y, int w, char c, uint8_t kind) {
    keys[keyCount++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)KB_H, c, kind};
  };
  const char* const* rows = KB_ROWS[kbLayer];
  for (int i = 0; i < 10; i++) add(i * 32, KB_Y, 32, rows[0][i], 0);
  for (int i = 0; i < 9; i++) add(16 + i * 32, KB_Y + KB_H, 32, rows[1][i], 0);
  add(0, KB_Y + 2 * KB_H, 48, 0, 1);
  for (int i = 0; i < 7; i++) add(48 + i * 32, KB_Y + 2 * KB_H, 32, rows[2][i], 0);
  add(272, KB_Y + 2 * KB_H, 48, 0, 2);
  add(0, KB_Y + 3 * KB_H, 64, 0, 3);
  add(64, KB_Y + 3 * KB_H, 160, ' ', 4);
  add(224, KB_Y + 3 * KB_H, 96, 0, 5);
}

bool passOk() {
  return strlen(joinPass) >= 8;
}

void drawKeyboard() {
  spr.fillScreen(C_BG);
  char head[80];
  if (wifiMsg[0]) txt(wifiMsg, 160, 18, F_SMB, C_RED, textdatum_t::middle_center, 300);
  else {
    snprintf(head, sizeof(head), "Password for %s", joinSsid);
    txt(head, 160, 18, F_SMB, C_TEXT, textdatum_t::middle_center, 300);
  }

  spr.fillSmoothRoundRect(10, PW_Y, 300, PW_H, 10, C_CARD);
  char shown[66];
  int n = strlen(joinPass);
  for (int i = 0; i < n; i++) shown[i] = kbShow ? joinPass[i] : '*';
  shown[n] = 0;
  spr.setFont(F_SMB);
  const char* vis = shown;
  while (*vis && spr.textWidth(vis) > 214) vis++;
  int cy = PW_Y + PW_H / 2;
  if (n) txt(vis, 22, cy + 1, F_SMB, C_TEXT, textdatum_t::middle_left);
  else txt("Type the password", 22, cy + 1, F_SMALL, C_DIM, textdatum_t::middle_left);
  if ((millis() / 500) % 2 == 0) {
    int cx = 22 + (n ? (int)spr.textWidth(vis) + 2 : 0);
    spr.setFont(F_SMB);
    spr.fillRect(cx, cy - 9, 2, 18, C_GREEN);
  }
  txt(kbShow ? "Hide" : "Show", 298, cy + 1, F_SMB, C_GREEN, textdatum_t::middle_right);

  buildKeys();
  bool flash = kbFlash >= 0 && millis() - kbFlashAt < 120;
  for (int i = 0; i < keyCount; i++) {
    const Key& k = keys[i];
    bool special = k.kind != 0 && k.kind != 4;
    uint16_t bg = (flash && kbFlash == i) ? C_GREEN : special ? C_CARD2 : C_CARD;
    uint16_t fg = (flash && kbFlash == i) ? RGB565(0, 0, 0) : C_TEXT;
    if (k.kind == 5) { bg = passOk() ? C_GREEN : C_CARD2; fg = passOk() ? RGB565(0, 0, 0) : C_DIM; }
    if (k.kind == 1 && kbLayer == 1) { bg = C_TEXT; fg = C_ON; }
    spr.fillSmoothRoundRect(k.x + 2, k.y + 3, k.w - 4, k.h - 6, 6, bg);
    int mx = k.x + k.w / 2, my = k.y + k.h / 2 + 1;
    char lab[2] = {k.c, 0};
    switch (k.kind) {
      case 0: txt(lab, mx, my, F_SMB, fg, textdatum_t::middle_center); break;
      case 1: txt(kbLayer < 2 ? "shift" : kbLayer == 2 ? "#+=" : "123", mx, my, F_TINY, fg, textdatum_t::middle_center); break;
      case 2:
        spr.fillTriangle(mx - 12, my - 1, mx - 5, my - 8, mx - 5, my + 6, fg);
        spr.fillRect(mx - 5, my - 8, 16, 15, fg);
        spr.drawLine(mx - 1, my - 4, mx + 6, my + 3, bg);
        spr.drawLine(mx + 6, my - 4, mx - 1, my + 3, bg);
        break;
      case 3: txt(kbLayer < 2 ? "123" : "abc", mx, my, F_SMB, fg, textdatum_t::middle_center); break;
      case 4: txt("space", mx, my, F_TINY, C_SUB, textdatum_t::middle_center); break;
      case 5: txt("Join", mx, my, F_SMB, fg, textdatum_t::middle_center); break;
    }
  }
}

void onKeyboardTap(int x, int y) {
  if (y >= PW_Y && y < PW_Y + PW_H) { if (x > 240) kbShow = !kbShow; return; }
  if (y < KB_Y) return;
  buildKeys();
  for (int i = 0; i < keyCount; i++) {
    const Key& k = keys[i];
    if (x < k.x || x >= k.x + k.w || y < k.y || y >= k.y + k.h) continue;
    kbFlash = i;
    kbFlashAt = millis();
    int n = strlen(joinPass);
    switch (k.kind) {
      case 0:
      case 4:
        if (n < 63) { joinPass[n] = k.c; joinPass[n + 1] = 0; }
        if (kbLayer == 1) kbLayer = 0;
        wifiMsg[0] = 0;
        break;
      case 1: kbLayer = kbLayer == 0 ? 1 : kbLayer == 1 ? 0 : kbLayer == 2 ? 3 : 2; break;
      case 2: if (n) joinPass[n - 1] = 0; break;
      case 3: kbLayer = kbLayer < 2 ? 2 : 0; break;
      case 5:
        if (passOk()) beginJoin();
        else setToast("Wi-Fi passwords are at least 8 characters");
        break;
    }
    return;
  }
}

// ---------------------------------------------------------------- overlays

void openOverlay(Overlay o) {
  overlay = o;
  ovShownKey = 0;
  ovTarget = 1;
  logoutArmedAt = 0;
  Serial.printf("[ui] overlay %s\n", o == OV_SETTINGS ? "settings" : "account");
}

void closeOverlay() {
  ovTarget = 0;
}

void updateOverlay(float dt) {
  if (overlay == OV_NONE) return;
  float k = min(1.0f, dt * 11);
  ovPos += (ovTarget - ovPos) * k;
  if (fabsf(ovTarget - ovPos) < 0.003f) ovPos = ovTarget;
  if (ovTarget == 0 && ovPos <= 0.002f) { overlay = OV_NONE; ovPos = 0; }
}

void goBack() {
  if (overlay != OV_NONE) { closeOverlay(); return; }
  switch (screen) {
    case SCR_LIST: goScreen(SCR_LIBRARY, -1); break;
    case SCR_NOW: openLibrary(-1); break;
    case SCR_LIBRARY: goScreen(SCR_NOW, -1); break;
    case SCR_WIFIPASS: wifiMsg[0] = 0; goScreen(SCR_WIFI, -1); break;
    case SCR_WIFI:
      if (WiFi.status() == WL_CONNECTED && wifiReturn != SCR_BOOT) goScreen(wifiReturn, -1);
      else if (wifiSsid[0] || WiFi.status() == WL_CONNECTED) { wifiMsg[0] = 0; bootStart = millis(); resumeSaved(); goScreen(SCR_BOOT, -1); }
      break;
    default: break;
  }
  Serial.println("[ui] back");
}

void handleBar(int cx, int y, uint16_t c) {
  spr.fillSmoothRoundRect(cx - 20, y, 40, 4, 2, c);
}

void segmented(int x, int y, int w, int h, const char* const* labels, int n, int sel, bool enabled) {
  spr.fillSmoothRoundRect(x, y, w, h, h / 2, C_CARD);
  int cw = w / n;
  for (int i = 0; i < n; i++) {
    int cx = x + i * cw;
    if (i == sel) spr.fillSmoothRoundRect(cx + 2, y + 2, cw - 4, h - 4, (h - 4) / 2, enabled ? C_GREEN : C_DIM);
    txt(labels[i], cx + cw / 2, y + h / 2 + 1, F_TINY, i == sel ? RGB565(0, 0, 0) : (enabled ? C_TEXT : C_DIM), textdatum_t::middle_center, cw - 4);
  }
}

void toggleSwitch(int x, int y, bool on) {
  spr.fillSmoothRoundRect(x, y, 40, 22, 11, on ? C_GREEN : C_CARD2);
  aaCircle(on ? x + 29.5f : x + 11.5f, y + 11, 8, on ? RGB565(255, 255, 255) : C_SUB);
}

const int SET_H = 24;
const int SET_ROW_THEME = 30, SET_ROW_BRIGHT = 58, SET_ROW_SLEEP = 86, SET_ROW_SAVER = 114, SET_ROW_TILES = 142, SET_ROW_BTN = 188;
const int BR_X = 112, BR_W = 152;
bool brightDrag = false;
const int SET_THEME_X = 170, SET_THEME_W = 136, SET_SLEEP_X = 100, SET_SLEEP_W = 206;
const int TILE_X = 14, TILE_W = 68, TILE_H = 34, TILE_GAP = 6, TILE_N = 4;
const uint16_t C_PINK = RGB565(0xFF, 0x4F, 0xA3);

void drawSaverPreview(int x, int y, int w, int h, uint8_t kind) {
  uint16_t bg = kind == 1 ? RGB565(250, 250, 250) : RGB565(0, 0, 0);
  uint16_t fg = kind == 1 ? RGB565(20, 20, 20) : kind == 2 ? C_GREEN : RGB565(255, 255, 255);
  spr.fillSmoothRoundRect(x, y, w, h, 5, bg);
  spr.fillRect(x + w / 2 - 10, y + 5, 20, 2, C_GREEN);
  txt("12:34", x + w / 2, y + h / 2 - (kind == 3 ? 2 : 0), F_SB12, fg, textdatum_t::middle_center);
  if (kind == 3) txt("Pink Propeller", x + w / 2, y + h - 5, F_R9, C_PINK, textdatum_t::middle_center);
  else spr.fillSmoothRoundRect(x + w / 2 - 14, y + h - 8, 28, 4, 2, kind == 1 ? RGB565(220, 220, 220) : RGB565(0x2A, 0x2A, 0x2A));
}

void drawSettings(int yo) {
  spr.fillRect(0, yo, 320, 240, C_BG);
  handleBar(160, yo + 231, C_DIM);
  txt("Settings", 14, yo + 12, F_SMB, C_TEXT);

  txt("Brightness", 14, yo + SET_ROW_BRIGHT + 12, F_SMALL, C_TEXT, textdatum_t::middle_left);
  float bf = (brightness - 20) / 235.0f;
  int by = yo + SET_ROW_BRIGHT + 10;
  spr.fillSmoothRoundRect(BR_X, by, BR_W, 4, 2, C_TRACK);
  int bx = BR_X + (int)(BR_W * bf);
  if (bx > BR_X + 2) spr.fillSmoothRoundRect(BR_X, by, bx - BR_X, 4, 2, C_GREEN);
  aaCircle(bx + 0.5f, by + 2, brightDrag ? 8.0f : 7.0f, RGB565(255, 255, 255));
  char pc[8];
  snprintf(pc, sizeof(pc), "%d%%", (int)(5 + bf * 95 + 0.5f));
  txt(pc, 306, yo + SET_ROW_BRIGHT + 12, F_SB11, C_SUB, textdatum_t::middle_right);

  static const char* const themeL[] = {"Dark", "Light"};
  txt("Theme", 14, yo + SET_ROW_THEME + SET_H / 2, F_SMALL, C_TEXT, textdatum_t::middle_left);
  segmented(SET_THEME_X, yo + SET_ROW_THEME, SET_THEME_W, SET_H, themeL, 2, themeLight ? 1 : 0, true);

  static const char* const sleepL[] = {"1m", "5m", "10m", "20m", "Never"};
  txt("Sleep", 14, yo + SET_ROW_SLEEP + SET_H / 2, F_SMALL, C_TEXT, textdatum_t::middle_left);
  segmented(SET_SLEEP_X, yo + SET_ROW_SLEEP, SET_SLEEP_W, SET_H, sleepL, 5, sleepIdx, true);

  bool never = SLEEP_MS[sleepIdx] == 0;
  txt("Screensaver", 14, yo + SET_ROW_SAVER + 11, F_SMALL, C_TEXT, textdatum_t::middle_left);
  if (never) txt("set a sleep time first", 118, yo + SET_ROW_SAVER + 11, F_TINY, C_DIM, textdatum_t::middle_left);
  toggleSwitch(266, yo + SET_ROW_SAVER, saverOn);

  for (int i = 0; i < TILE_N; i++) {
    int x = TILE_X + i * (TILE_W + TILE_GAP), y = yo + SET_ROW_TILES;
    if (i == saverKind) spr.fillSmoothRoundRect(x - 3, y - 3, TILE_W + 6, TILE_H + 6, 8, C_GREEN);
    spr.fillSmoothRoundRect(x - 1, y - 1, TILE_W + 2, TILE_H + 2, 6, i == saverKind ? C_BG : C_CARD2);
    drawSaverPreview(x, y, TILE_W, TILE_H, i);
    if (!saverOn || never) dimRect(x - 3, y - 3, TILE_W + 6, TILE_H + 6, C_BG, 0.55f);
  }

  bool armed = logoutArmedAt && millis() - logoutArmedAt < 3000;
  bool up = WiFi.status() == WL_CONNECTED;
  int ly = yo + SET_ROW_BTN;
  spr.fillSmoothRoundRect(14, ly, 142, 28, 14, C_CARD);
  wifiBars(26, ly + 14, up ? WiFi.RSSI() : -100, up ? C_GREEN : C_DIM, C_CARD2);
  String ss = up ? WiFi.SSID() : String("Wi-Fi");
  txt(ss.c_str(), 54, ly + 15, F_SMALL, C_TEXT, textdatum_t::middle_left, 96);
  spr.fillSmoothRoundRect(164, ly, 142, 28, 14, armed ? C_RED : C_CARD);
  txt(armed ? "Tap again" : (loggedIn() ? "Log out" : "Not logged in"), 235, ly + 15, F_SMALL, armed ? RGB565(255, 255, 255) : C_RED, textdatum_t::middle_center, 130);
}

void drawAccount(int yo) {
  spr.fillRect(0, yo, 320, 240, C_BG);
  for (int y = 0; y < 76; y += 3) spr.fillRect(0, yo + y, 320, 3, mix(C_DKGREEN, C_BG, y / 76.0f));
  int cx = 42, cy = yo + 40, r = 26;
  if (avatarReady) {
    avatarSpr.pushRotateZoom(&spr, cx, cy, 0, (2.0f * r) / 64, (2.0f * r) / 64);
    spr.fillArc(cx, cy, r, r + 12, 0, 360, mix(C_DKGREEN, C_BG, 0.75f));
  } else {
    aaCircle(cx + 0.5f, cy + 0.5f, r, C_GREEN);
    char ini[2] = {myName[0] ? (char)toupper(myName[0]) : '?', 0};
    txt(ini, cx, cy + 2, F_MED, RGB565(0, 0, 0), textdatum_t::middle_center);
  }
  txt("Account", 82, yo + 20, F_TINY, C_SUB);
  txt(myName[0] ? myName : "Not logged in", 82, yo + 36, F_SMB, C_TEXT, textdatum_t::top_left, 224);

  char line[96], status[48];
  bool up = WiFi.status() == WL_CONNECTED;
  int code = lastPlayerCode;
  if (!up) snprintf(status, sizeof(status), "No Wi-Fi");
  else if (code == 200 && lastGoodPoll) snprintf(status, sizeof(status), "In sync (%us ago)", (unsigned)((millis() - lastGoodPoll) / 1000));
  else if (code == 204 || code == 202) snprintf(status, sizeof(status), "Connected, nothing playing");
  else if (code == 429) snprintf(status, sizeof(status), "Busy, retrying");
  else if (code == 0) snprintf(status, sizeof(status), "Starting...");
  else snprintf(status, sizeof(status), "Error %d", code);

  spr.fillSmoothRoundRect(10, yo + 82, 300, 134, 10, C_CARD);
  int y = yo + 90;
  auto row = [&](const char* label, const char* value, uint16_t vc) {
    txt(label, 22, y, F_TINY, C_SUB);
    txt(value, 124, y, F_TINY, vc, textdatum_t::top_left, 178);
    y += 21;
  };
  row("Spotify", status, code == 200 || code == 204 ? C_GREEN : C_TEXT);
  row("Playing on", snap.active && snap.device[0] ? snap.device : "Nothing playing", C_TEXT);
  snprintf(line, sizeof(line), "%d", (int)plCount);
  row("Playlists", line, C_TEXT);
  String ss = WiFi.SSID();
  row("Wi-Fi", up ? ss.c_str() : "Not connected", C_TEXT);
  row("Address", up ? WiFi.localIP().toString().c_str() : "-", C_TEXT);
  txt("Swipe down to close", 160, yo + 229, F_TINY, C_DIM, textdatum_t::middle_center);
}


uint32_t overlayKey() {
  bool up = WiFi.status() == WL_CONNECTED;
  uint32_t k = hk(hk(hk(overlay, themeLight), up), wifiLevel(up));
  if (overlay == OV_SETTINGS) {
    bool armed = logoutArmedAt && millis() - logoutArmedAt < 3000;
    k = hk(hk(hk(hk(hk(hk(hk(k, sleepIdx), saverOn), saverKind), armed), loggedIn()), brightness), brightDrag);
  } else {
    k = hk(hk(hk(hk(k, avatarReady), lastPlayerCode), lastGoodPoll ? (millis() - lastGoodPoll) / 1000 : 0), plCount);
    k = hs(hs(hs(k, myName), snap.active ? snap.device : ""), likePlName);
  }
  return k;
}

void prepareOverlay() {
  if (overlay == OV_NONE) return;
  uint32_t k = overlayKey();
  if (k == ovShownKey) return;
  if (overlay == OV_SETTINGS) drawSettings(0);
  else drawAccount(0);
  memcpy(ovSpr.getBuffer(), spr.getBuffer(), 320 * 240 * 2);
  ovShownKey = k;
}

bool overlayCovers() {
  return overlay != OV_NONE && ovPos >= 0.999f;
}

void drawOverlay() {
  if (overlay == OV_NONE) return;
  float e = easeOutCubic(ovPos);
  ovSpr.pushSprite(&spr, 0, overlay == OV_SETTINGS ? (int)(-240 * (1 - e)) : (int)(240 * (1 - e)));
}

void onOverlayTap(int x, int y) {
  if (ovPos < 0.9f) return;
  if (overlay != OV_SETTINGS) return;
  if (y >= SET_ROW_BRIGHT - 2 && y < SET_ROW_BRIGHT + 26) return;
  if (y >= SET_ROW_THEME - 4 && y < SET_ROW_THEME + SET_H + 2 && x >= SET_THEME_X) {
    bool light = x >= SET_THEME_X + SET_THEME_W / 2;
    if (light != themeLight) { applyTheme(light); saveUiPrefs(); setToast(light ? "Light mode" : "Dark mode"); }
    return;
  }
  if (y >= SET_ROW_SLEEP - 2 && y < SET_ROW_SLEEP + SET_H + 3 && x >= SET_SLEEP_X) {
    int i = constrain((x - SET_SLEEP_X) / (SET_SLEEP_W / 5), 0, 4);
    if (i != sleepIdx) { sleepIdx = i; saveUiPrefs(); }
    return;
  }
  if (y >= SET_ROW_SAVER - 1 && y < SET_ROW_SAVER + 25) {
    saverOn = !saverOn;
    saveUiPrefs();
    if (saverOn && SLEEP_MS[sleepIdx] == 0) setToast("Pick a sleep time so the screensaver can start");
    return;
  }
  if (y >= SET_ROW_TILES - 3 && y < SET_ROW_TILES + TILE_H + 4) {
    int i = (x - TILE_X) / (TILE_W + TILE_GAP);
    if (i >= 0 && i < TILE_N) { saverKind = i; saverOn = true; saveUiPrefs(); }
    return;
  }
  if (y >= SET_ROW_BTN - 4 && y < SET_ROW_BTN + 32) {
    if (x < 160) {
      overlay = OV_NONE;
      ovPos = ovTarget = 0;
      openWifi(false);
      return;
    }
    if (!loggedIn()) return;
    if (logoutArmedAt && millis() - logoutArmedAt < 3000) {
      logoutArmedAt = 0;
      logout();
      closeOverlay();
      setToast("Logged out");
    } else {
      logoutArmedAt = millis();
    }
  }
}

// ---------------------------------------------------------------- screensaver

int saverEqX = -1, saverEqY = 0;
uint16_t saverPill = 0;
LGFX_Sprite saverSpr(&lcd);
uint32_t saverShownKey = 0;

void drawSaverFrame() {
  char t[12];
  fmtClock(t, sizeof(t));
  uint32_t k = hs(hs(hs(hs(hk(hk(hk(hk(saverKind, (millis() - saverStart) / 60000), snap.active), clock24), snap.playing), t), snap.title), snap.artist), lastTitle);
  if (k != saverShownKey) {
    saverEqX = -1;
    drawScreensaver();
    memcpy(saverSpr.getBuffer(), spr.getBuffer(), 320 * 240 * 2);
    saverShownKey = k;
  } else {
    memcpy(spr.getBuffer(), saverSpr.getBuffer(), 320 * 240 * 2);
  }
  if (saverEqX >= 0 && snap.playing) {
    spr.fillRect(saverEqX, saverEqY, 13, 12, saverPill);
    eqBars(saverEqX + 2, saverEqY + 10, C_GREEN, true);
  }
}

void drawScreensaver() {
  uint8_t k = saverKind;
  uint16_t bg = k == 1 ? RGB565(250, 250, 250) : RGB565(0, 0, 0);
  uint16_t fg = k == 1 ? RGB565(20, 20, 20) : k == 2 ? C_GREEN : RGB565(255, 255, 255);
  uint16_t pill = k == 1 ? RGB565(232, 232, 232) : RGB565(0x1A, 0x1A, 0x1A);
  uint16_t ptxt = k == 1 ? RGB565(20, 20, 20) : RGB565(255, 255, 255);
  uint16_t psub = k == 1 ? RGB565(110, 110, 110) : C_SUB;
  spr.fillScreen(bg);

  uint32_t mins = (millis() - saverStart) / 60000;
  int dx = (int)(mins % 7) - 3, dy = (int)((mins / 7) % 5) - 2;
  int top = k == 3 ? -6 : 0;

  char t[12], d[48];
  if (fmtClock(t, sizeof(t))) {
    if (fmtDate(d, sizeof(d))) txt(d, 160 + dx, 49 + top + dy, F_R11, C_GREEN, textdatum_t::middle_center);
    txt(t, 160 + dx, 105 + top + dy, F_L92, fg, textdatum_t::middle_center);
  } else {
    txt("Pink Propeller", 160 + dx, 105 + dy, F_B22, fg, textdatum_t::middle_center);
  }

  const char* title = snap.title[0] ? snap.title : lastTitle;
  const char* artist = snap.title[0] ? snap.artist : lastArtist;
  if (title[0]) {
    char ti[96], ar[96];
    spr.setFont(F_R11);
    fitText(ti, sizeof(ti), title, 150);
    int tw = spr.textWidth(ti);
    fitText(ar, sizeof(ar), artist, max(30, 270 - 26 - tw - 6 - 12));
    int aw = spr.textWidth(ar);
    int w = 26 + tw + 6 + aw + 12;
    int x0 = 160 - w / 2, y0 = 174 + top + dy;
    spr.fillSmoothRoundRect(x0, y0, w, 24, 12, pill);
    eqBars(x0 + 11, y0 + 16, snap.playing ? C_GREEN : psub, false);
    saverEqX = x0 + 9;
    saverEqY = y0 + 6;
    saverPill = pill;
    txt(ti, x0 + 26, y0 + 12, F_R11, ptxt, textdatum_t::middle_left);
    txt(ar, x0 + 26 + tw + 6, y0 + 12, F_R11, psub, textdatum_t::middle_left);
  }
  if (k == 3) txt("Pink Propeller", 160 + dx, 220 + dy, F_B16, C_PINK, textdatum_t::middle_center);
}

// ---------------------------------------------------------------- input

bool hit(int x, int y, int cx, int cy, int rx, int ry) {
  return abs(x - cx) <= rx && abs(y - cy) <= ry;
}

void setBrightnessFromX(int x) {
  float f = constrain((float)(x - BR_X) / BR_W, 0.0f, 1.0f);
  uint8_t b = 20 + (uint8_t)(f * 235 + 0.5f);
  if (b != brightness) {
    brightness = b;
    lcd.setBrightness(brightness);
  }
}

void enterStandby() {
  if (overlay != OV_NONE) { overlay = OV_NONE; ovPos = ovTarget = 0; }
  saverActive = true;
  saverStart = millis();
  lcd.setBrightness(min(70, (int)brightness));
  Serial.println("[power] standby");
}

bool topBarTap(int x, int y, uint8_t left) {
  if (y > 28) return false;
  if (x < 42) {
    if (left == 0) openLibrary(-1);
    else if (left == 2) goScreen(SCR_LIBRARY, -1);
    return true;
  }
  if (abs(x - 160) < 36) {
    clock24 = !clock24;
    saveUiPrefs();
    setToast(clock24 ? "24-hour clock" : "12-hour clock");
    return true;
  }
  if (x >= 266 && x < 290) { openWifi(false); return true; }
  if (x >= 290) { enterStandby(); return true; }
  return true;
}

void onTap(int x, int y) {
  Serial.printf("[touch] tap %d,%d on screen %d\n", x, y, (int)screen);
  if (overlay != OV_NONE) { onOverlayTap(x, y); return; }
  bool mini = snap.active && snap.title[0];
  switch (screen) {
    case SCR_BOOT:
      if (!joining) openWifi(false);
      break;
    case SCR_WIFI: {
      if (y < WIFI_TOP) { if (x > 240) startScan(); return; }
      int i = (y - WIFI_TOP + (int)wifiScroll.y) / WIFI_ROW;
      if (i >= 0 && i < netCount) pickNetwork(i);
      break;
    }
    case SCR_WIFIPASS:
      onKeyboardTap(x, y);
      break;
    case SCR_IDLE:
      goScreen(snap.active && snap.playing ? SCR_NOW : SCR_LIBRARY, 1);
      if (!plLoadedOnce && !plLoading) sendCmd(C_RELOAD_PL);
      break;
    case SCR_LIBRARY: {
      if (topBarTap(x, y, 1)) return;
      if (y >= TAB_Y - 5 && y < TAB_Y + TAB_H + 4) {
        for (int i = 0; i < 2; i++) {
          if (x >= tabX[i] - 3 && x < tabX[i] + tabW[i] + 3 && libTab != i) {
            libTab = i;
            libScroll = Scroller();
            if (i == 0 && recentState != RS_LOADING) needRecent = true;
          }
        }
        return;
      }
      if (mini && y >= MINI_Y) {
        if (x >= 248 && x < 281) actTogglePlay();
        else if (x >= 281) actSkip(1);
        else goScreen(SCR_NOW, 1);
        return;
      }
      if (y < LIB_TOP) return;
      if (libTab == 0) {
        if (recentCount == 0 && recentState == RS_ERROR) { needRecent = true; return; }
        int col = x >= 160 ? 1 : 0, row = (y - LIB_TOP) / RT_ROW;
        if ((y - LIB_TOP) % RT_ROW < RT_H) openRecent(row * 2 + col);
      } else {
        int row = (int)((y - LIB_TOP + libScroll.y) / LIB_ROW);
        if (row == 0) openList("liked", "Liked Songs", -1, 0, LK_LIKED, "You", -1);
        else openPlaylistRow(row - 1);
      }
      break;
    }
    case SCR_LIST: {
      if (topBarTap(x, y, 2)) return;
      if (y < TR_TOP) {
        if (hit(x, y, 289, 56, 22, 22)) actPlayAt(0, false);
        else if (hit(x, y, 245, 56, 16, 16)) actPlayAt(0, true);
        return;
      }
      if (trError && !trLoading && trCount == 0) { reopenList(); return; }
      int idx = (int)((y - TR_TOP + listScroll.y) / TR_ROW);
      LOCK();
      bool ok = idx >= 0 && idx < trCount && trs[idx].uri[0];
      UNLOCK();
      if (ok) actPlayAt(idx, false);
      break;
    }
    case SCR_NOW: {
      if (topBarTap(x, y, 0)) return;
      bool has = snap.active && snap.title[0];
      if (!has) {
        if (y > 30 && y < 140) openLibrary(-1);
        return;
      }
      if (abs(y - CTRL_Y) < 26) {
        if (hit(x, y, CX_PLAY, CTRL_Y, 28, 26)) actTogglePlay();
        else if (hit(x, y, CX_PREV, CTRL_Y, 28, 24)) actPrev();
        else if (hit(x, y, CX_NEXT, CTRL_Y, 28, 24)) actSkip(1);
        else if (hit(x, y, CX_SHUF, CTRL_Y, 28, 24)) actShuffle();
        else if (hit(x, y, CX_REP, CTRL_Y, 28, 24)) actRepeat();
        return;
      }
      if (abs(y - PB_Y) < 12 && x >= PB_X - 6 && x <= PB_X + PB_W + 6 && snap.duration) {
        actSeek((float)(x - PB_X) / PB_W);
      }
      break;
    }
    default: break;
  }
}

void onSwipe(int dx, int dy) {
  bool horiz = abs(dx) > abs(dy) * 1.5f;
  bool vert = abs(dy) > abs(dx) * 1.5f;
  if (edgeKind == 3 && horiz && dx > 60) { goBack(); return; }
  if (overlay == OV_SETTINGS) { if (vert && dy < -60) closeOverlay(); return; }
  if (overlay == OV_ACCOUNT) { if (vert && dy > 60) closeOverlay(); return; }
  bool ready = screen != SCR_BOOT && screen != SCR_WIFIPASS;
  if (edgeKind == 1 && vert && dy < -45 && ready) { openOverlay(OV_ACCOUNT); return; }
  if (edgeKind == 2 && vert && dy > 45 && ready) { openOverlay(OV_SETTINGS); return; }
  if (screen == SCR_NOW && horiz && abs(dx) > 60 && tp.sy > 30 && tp.sy < 170 && snap.active) {
    actSkip(dx < 0 ? 1 : -1);
  }
}

Scroller* activeScroller() {
  if (overlay != OV_NONE || edgeKind) return nullptr;
  if (screen == SCR_LIBRARY) return &libScroll;
  if (screen == SCR_LIST) return &listScroll;
  if (screen == SCR_WIFI) return &wifiScroll;
  return nullptr;
}

bool dozing() {
  return screenAsleep || saverActive;
}

void wake() {
  lastInteraction = millis();
  pollNow = true;
  if (saverActive) {
    saverActive = false;
    lcd.setBrightness(brightness);
    if (loggedIn() && (screen == SCR_LIBRARY || screen == SCR_LIST || screen == SCR_IDLE)) goScreen(SCR_NOW, 0);
    Serial.println("[power] screensaver off");
  }
  if (screenAsleep) {
    screenAsleep = false;
    lcd.wakeup();
    lcd.setBrightness(brightness);
    shownValid = false;
    Serial.println("[power] screen on");
  }
}

void goToSleep() {
  if (dozing()) return;
  if (overlay != OV_NONE) { overlay = OV_NONE; ovPos = ovTarget = 0; }
  if (saverOn) {
    saverActive = true;
    saverStart = millis();
    screenAsleep = false;
    lcd.setBrightness(min(70, (int)brightness));
    Serial.println("[power] screensaver on");
  } else {
    screenAsleep = true;
    lcd.setBrightness(0);
    lcd.sleep();
    Serial.println("[power] screen off");
  }
}

void handleTouch(float dt) {
  int32_t x, y;
  bool now = lcd.getTouch(&x, &y);
  uint32_t ms = millis();
  if (now) {
    tp.lastSeen = ms;
    if (!tp.down) {
      tp.down = true;
      tp.moved = false;
      tp.sx = tp.x = x;
      tp.sy = tp.y = y;
      tp.py = y;
      tp.t0 = ms;
      tp.vel = 0;
      tp.target = screen == SCR_LIST ? 2 : 1;
      if (dozing()) { wake(); tp.swallow = true; return; }
      tp.swallow = false;
      touchActivity();
      edgeKind = 0;
      if (screen == SCR_WIFIPASS) {
        if (x < EDGE_PX) edgeKind = 3;
      } else if (overlay == OV_NONE) {
        if (y > 240 - EDGE_PX) edgeKind = 1;
        else if (y < EDGE_PX) edgeKind = 2;
        else if (x < EDGE_PX) edgeKind = 3;
      } else if (x < EDGE_PX) {
        edgeKind = 3;
      }
      if (overlay == OV_SETTINGS && ovPos > 0.9f && y >= SET_ROW_BRIGHT - 4 && y < SET_ROW_BRIGHT + 28 && x >= BR_X - 14) {
        brightDrag = true;
        setBrightnessFromX(x);
        return;
      }
      Scroller* s = activeScroller();
      if (s) s->vel = 0;
      if (overlay == OV_NONE && !edgeKind && screen == SCR_NOW && snap.duration && abs(y - PB_Y) < 14 && x >= PB_X - 6 && x <= PB_X + PB_W + 6) {
        seeking = true;
        seekFrac = constrain((float)(x - PB_X) / PB_W, 0.0f, 1.0f);
      }
      return;
    }
    if (tp.swallow) return;
    tp.x = x;
    tp.y = y;
    if (abs(x - tp.sx) > 9 || abs(y - tp.sy) > 9) tp.moved = true;
    touchActivity();
    if (seeking) {
      seekFrac = constrain((float)(x - PB_X) / PB_W, 0.0f, 1.0f);
      return;
    }
    if (brightDrag) {
      setBrightnessFromX(x);
      return;
    }
    Scroller* s = activeScroller();
    if (s && tp.moved) {
      float d = (float)(tp.py - y);
      if (s->y < 0 || s->y > s->maxY) d *= 0.4f;
      s->y += d;
      if (dt > 0) tp.vel = tp.vel * 0.5f + (d / dt) * 0.5f;
    }
    tp.py = y;
    return;
  }
  if (tp.down && ms - tp.lastSeen > 45) {
    tp.down = false;
    if (tp.swallow) { tp.swallow = false; return; }
    if (seeking) {
      seeking = false;
      actSeek(seekFrac);
      return;
    }
    if (brightDrag) {
      brightDrag = false;
      saveUiPrefs();
      edgeKind = 0;
      return;
    }
    if (screen == SCR_WIFIPASS && overlay == OV_NONE && !(edgeKind == 3 && tp.x - tp.sx > 60)) {
      onTap(tp.sx, tp.sy);
      edgeKind = 0;
      return;
    }
    Scroller* s = activeScroller();
    if (!tp.moved) onTap(tp.sx, tp.sy);
    else {
      if (s) s->vel = constrain(tp.vel, -2500.0f, 2500.0f);
      if (!s || abs(tp.x - tp.sx) > abs(tp.y - tp.sy)) onSwipe(tp.x - tp.sx, tp.y - tp.sy);
    }
    edgeKind = 0;
  }
}

void updateScroller(Scroller& s, float dt) {
  if (tp.down && tp.moved) return;
  s.y += s.vel * dt;
  s.vel *= powf(0.03f, dt);
  if (fabsf(s.vel) < 5) s.vel = 0;
  if (s.y < 0) { s.y += (0 - s.y) * min(1.0f, dt * 12); s.vel *= 0.5f; if (s.y > -0.5f) s.y = 0; }
  if (s.y > s.maxY) { s.y += (s.maxY - s.y) * min(1.0f, dt * 12); s.vel *= 0.5f; if (s.y < s.maxY + 0.5f) s.y = s.maxY; }
}

int32_t encConsumed = 0;

void handleEncoder() {
  int32_t raw = encRaw;
  int32_t moved = raw - encConsumed;
  int up = ENC_REVERSE ? -1 : 1;
  while (moved >= ENC_STEPS_PER_DETENT || moved <= -ENC_STEPS_PER_DETENT) {
    int dir = moved > 0 ? up : -up;
    encConsumed += moved > 0 ? ENC_STEPS_PER_DETENT : -ENC_STEPS_PER_DETENT;
    moved = raw - encConsumed;
    if (dozing()) { wake(); encConsumed = raw; return; }
    touchActivity();
    if (loggedIn()) bumpVolume(dir);
  }
}

// ---------------------------------------------------------------- frame

LGFX_Sprite tmpSpr(&lcd);

bool decodeScaled(LGFX_Sprite& dst, int size, const uint8_t* buf, int len) {
  tmpSpr.fillScreen(C_CARD2);
  if (!tmpSpr.drawJpg(buf, len, 0, 0, 150, 150, 0, 0, -1.0f, -1.0f)) return false;
  dst.fillScreen(C_CARD2);
  tmpSpr.pushRotateZoomWithAA(&dst, size / 2.0f - 0.5f, size / 2.0f - 0.5f, 0, size / 150.0f, size / 150.0f);
  return true;
}

void decodePendingImages() {
  if (avatarPending) {
    avatarSpr.fillScreen(C_GREEN);
    avatarReady = avatarSpr.drawJpg(avatarBuf, avatarBufLen, 0, 0, 64, 64, 0, 0, -1.0f, -1.0f);
    avatarPending = false;
  }
  if (artPending) {
    bool ok = decodeScaled(artSpr, ART, artBuf, artBufLen);
    if (ok) {
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int y = 4; y < ART; y += 12) {
        for (int x = 4; x < ART; x += 12) {
          auto c = artSpr.readPixelRGB(x, y);
          r += c.r; g += c.g; b += c.b; n++;
        }
      }
      float k = 0.55f;
      artTint = RGB565(min(255.0f, r / n * k), min(255.0f, g / n * k), min(255.0f, b / n * k));
      copyStr(artShownUrl, sizeof(artShownUrl), artFetchedUrl);
      artGen++;
    }
    artPending = false;
  }
  int idx = thumbBufIdx;
  if (idx >= 0) {
    if (!thumbs[idx]) {
      thumbs[idx] = new LGFX_Sprite(&lcd);
      thumbs[idx]->setColorDepth(16);
      thumbs[idx]->setPsram(true);
      thumbs[idx]->createSprite(THUMB, THUMB);
    }
    bool ok = decodeScaled(*thumbs[idx], THUMB, thumbBuf, thumbBufLen);
    thumbState[idx] = ok ? 4 : 5;
    thumbGen[idx]++;
    thumbBufIdx = -1;
  }
}

void updateAutoScreens() {
  uint32_t now = millis();
  pollScan();
  bool up = WiFi.status() == WL_CONNECTED;
  if (joining) {
    if (up) {
      joining = false;
      copyStr(wifiSsid, sizeof(wifiSsid), joinSsid);
      copyStr(wifiPass, sizeof(wifiPass), joinOpen ? "" : joinPass);
      saveWifi();
      wifiMsg[0] = 0;
      char m[80];
      snprintf(m, sizeof(m), "Connected to %s", wifiSsid);
      setToast(m);
      Serial.printf("[wifi] saved %s\n", wifiSsid);
      if (wifiReturn != SCR_BOOT && wifiReturn != SCR_WIFI && wifiReturn != SCR_WIFIPASS) { goScreen(wifiReturn, 0); return; }
    } else if (now - joinStart > 15000) {
      joining = false;
      WiFi.disconnect();
      if (joinOpen) snprintf(wifiMsg, sizeof(wifiMsg), "Couldn't join %s", joinSsid);
      else snprintf(wifiMsg, sizeof(wifiMsg), "Couldn't join. Check the password.");
      Serial.printf("[wifi] join %s failed\n", joinSsid);
      resumeSaved();
      goScreen(joinOpen ? SCR_WIFI : SCR_WIFIPASS, 0);
      return;
    } else {
      return;
    }
  }
  if (screen == SCR_WIFI || screen == SCR_WIFIPASS) {
    if (screen == SCR_WIFI && wifiAuto && up) { wifiAuto = false; wifiMsg[0] = 0; goScreen(SCR_BOOT, 0); }
    else {
      if (screen == SCR_WIFI && !scanning && now - scanAt > 15000) startScan();
      return;
    }
  }
  if (screen == SCR_BOOT) {
    if (!up) {
      if (!wifiSsid[0]) openWifi(true);
      else if (now - bootStart > 15000) {
        snprintf(wifiMsg, sizeof(wifiMsg), "Can't reach %s", wifiSsid);
        openWifi(true);
      }
      return;
    }
    goScreen(loggedIn() ? SCR_NOW : SCR_SETUP, 0);
    return;
  }
  if (!loggedIn() && screen != SCR_SETUP) { goScreen(SCR_SETUP, 0); return; }
  if (loggedIn() && (screen == SCR_SETUP || screen == SCR_IDLE)) { goScreen(SCR_NOW, 0); return; }

  if (snap.playing) lastPlayingAt = now;
  if (snap.title[0]) {
    copyStr(lastTitle, sizeof(lastTitle), snap.title);
    copyStr(lastArtist, sizeof(lastArtist), snap.artist);
  }
  if (snap.playing && !wasPlaying) {
    bool was = dozing();
    wake();
    if (was || screen == SCR_IDLE) goScreen(SCR_NOW, 0);
  }
  if (snap.title[0] && strcmp(snap.trackId, "") != 0) {
    static char lastId[40] = "";
    if (strcmp(lastId, snap.trackId) != 0) {
      copyStr(lastId, sizeof(lastId), snap.trackId);
      marqueeStart = now;
    }
  }
  wasPlaying = snap.playing;

  uint32_t quiet = now - max(lastInteraction, lastPlayingAt);
  uint32_t limit = SLEEP_MS[sleepIdx];
  if (limit && !snap.playing && !tp.down && quiet > limit) goToSleep();
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println();
  Serial.println("##### CAR THING for ESP32-S3 #####");

  mux = xSemaphoreCreateRecursiveMutex();
  cmdQ = xQueueCreate(16, sizeof(Cmd));

  pls = (PL*)heap_caps_calloc(MAX_PL, sizeof(PL), MALLOC_CAP_SPIRAM);
  trs = (TR*)heap_caps_calloc(MAX_TR, sizeof(TR), MALLOC_CAP_SPIRAM);
  artBuf = (uint8_t*)heap_caps_malloc(200000, MALLOC_CAP_SPIRAM);
  thumbBuf = (uint8_t*)heap_caps_malloc(150000, MALLOC_CAP_SPIRAM);
  avatarBuf = (uint8_t*)heap_caps_malloc(60000, MALLOC_CAP_SPIRAM);
  if (!pls || !trs || !artBuf || !thumbBuf) Serial.println("[mem] PSRAM allocation FAILED - set Tools > PSRAM to OPI PSRAM");
  for (int i = 0; i < NTHUMB; i++) { thumbs[i] = nullptr; thumbState[i] = 0; }

  loadSettings();
  if (loggedIn()) needPlaylists = true;
  Serial.printf("[auth] %s\n", loggedIn() ? "saved login found" : "not logged in yet");

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  delay(5);
  Serial.printf("[enc] A=IO%d B=IO%d levels %d %d\n", ENC_A, ENC_B, digitalRead(ENC_A), digitalRead(ENC_B));

  pinMode(18, OUTPUT);
  digitalWrite(18, LOW);
  delay(10);
  digitalWrite(18, HIGH);
  delay(200);

  loadFonts();
  lcd.init();
  lcd.setRotation(SCREEN_ROTATION);
  lcd.setBrightness(brightness);
  lcd.fillScreen(C_BG);

  for (LGFX_Sprite* s : {&spr, &prevSpr}) {
    s->setColorDepth(16);
    s->setPsram(true);
    if (!s->createSprite(320, 240)) Serial.println("[lcd] frame buffer FAILED");
  }
  artSpr.setColorDepth(16);
  artSpr.setPsram(true);
  artSpr.createSprite(ART, ART);
  for (LGFX_Sprite* s : {&shownSpr, &ovSpr, &saverSpr}) {
    s->setColorDepth(16);
    s->setPsram(true);
    if (!s->createSprite(320, 240)) Serial.println("[lcd] extra buffer FAILED");
  }
  tmpSpr.setColorDepth(16);
  tmpSpr.setPsram(true);
  tmpSpr.createSprite(150, 150);
  avatarSpr.setColorDepth(16);
  avatarSpr.setPsram(true);
  avatarSpr.createSprite(64, 64);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(DEVICE_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  if (wifiSsid[0]) {
    WiFi.begin(wifiSsid, wifiPass);
    Serial.printf("[wifi] connecting to saved network %s\n", wifiSsid);
  } else {
    Serial.println("[wifi] no saved network - pick one on the screen");
  }
  bootStart = millis();

  xTaskCreatePinnedToCore(encoderTask, "enc", 2048, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(webTask, "web", 12288, nullptr, 1, nullptr, 0);

  lastInteraction = millis();
}

uint32_t lastFrame = 0, lastWifiLog = 0;
bool wifiWasUp = false;


void presentFrame() {
  uint16_t* a = (uint16_t*)spr.getBuffer();
  uint16_t* b = (uint16_t*)shownSpr.getBuffer();
  if (!shownValid) {
    spr.pushSprite(&lcd, 0, 0);
    memcpy(b, a, 320 * 240 * 2);
    shownValid = true;
    return;
  }
  int y = 0;
  lcd.startWrite();
  while (y < 240) {
    while (y < 240 && memcmp(a + y * 320, b + y * 320, 640) == 0) y++;
    if (y >= 240) break;
    int top = y, last = y, gap = 0;
    int x0 = 320, x1 = -1;
    while (y < 240 && gap < 6) {
      uint16_t* ra = a + y * 320;
      uint16_t* rb = b + y * 320;
      if (memcmp(ra, rb, 640) != 0) {
        int l = 0, r = 319;
        while (ra[l] == rb[l]) l++;
        while (ra[r] == rb[r]) r--;
        if (l < x0) x0 = l;
        if (r > x1) x1 = r;
        last = y;
        gap = 0;
      } else {
        gap++;
      }
      y++;
    }
    int h = last - top + 1, w = x1 - x0 + 1;
    lcd.setClipRect(x0, top, w, h);
    spr.pushSprite(&lcd, 0, 0);
    lcd.clearClipRect();
    for (int r = top; r <= last; r++) memcpy(b + r * 320 + x0, a + r * 320 + x0, w * 2);
  }
  lcd.endWrite();
}

void loop() {
  uint32_t now = millis();
  float dt = min((now - lastFrame) / 1000.0f, 0.05f);
  lastFrame = now;

  bool up = WiFi.status() == WL_CONNECTED;
  if (up != wifiWasUp) {
    wifiWasUp = up;
    if (up) Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    else Serial.println("[wifi] disconnected");
  }
  if (!up && now - lastWifiLog > 5000) {
    lastWifiLog = now;
    Serial.printf("[wifi] waiting for %s (status %d)\n", wifiSsid[0] ? wifiSsid : "a network", (int)WiFi.status());
  }

  LOCK();
  snap = shared;
  UNLOCK();
  if (snap.volume >= 0 && now - lastLocalVol > 2500) uiVol = snap.volume;

  handleEncoder();
  handleTouch(dt);
  updateAutoScreens();

  if (screenAsleep) { delay(20); return; }

  decodePendingImages();
  updateOverlay(dt);
  if (saverActive) {
    drawSaverFrame();
    drawVolumeOverlay(dt);
    presentFrame();
    delay(20);
    return;
  }
  saverShownKey = 0;
  if (screen == SCR_LIBRARY) updateScroller(libScroll, dt);
  if (screen == SCR_LIST) updateScroller(listScroll, dt);

  prepareOverlay();
  if (!overlayCovers()) switch (screen) {
    case SCR_BOOT: drawBoot(); break;
    case SCR_SETUP: drawSetup(); break;
    case SCR_IDLE: drawIdle(); break;
    case SCR_LIBRARY: drawLibrary(); break;
    case SCR_LIST: drawList(); break;
    case SCR_NOW: drawNow(); break;
    case SCR_WIFI: updateScroller(wifiScroll, dt); drawWifi(); break;
    case SCR_WIFIPASS: drawKeyboard(); break;
  }
  drawOverlay();
  drawVolumeOverlay(dt);
  drawToast();

  if (transActive) {
    float t = (now - transStart) / 260.0f;
    if (t >= 1) {
      transActive = false;
      shownValid = false;
      presentFrame();
    } else {
      float e = easeOutCubic(t);
      int nx = transDir > 0 ? (int)(320 * (1 - e)) : (int)(-320 * (1 - e));
      int px = transDir > 0 ? nx - 320 : nx + 320;
      prevSpr.pushSprite(&lcd, px, 0);
      spr.pushSprite(&lcd, nx, 0);
      shownValid = false;
    }
  } else {
    presentFrame();
  }
  delay(1);
}
