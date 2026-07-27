// vm-switcher para Waveshare ESP32-S3-Touch-LCD-7C.
// Panel tactil de 7" (800x480) que controla las VMs/CTs del host Proxmox a
// traves de vm-switcher-api (http://192.168.1.10:8088).
// Stack: LovyanGFX (panel RGB) + LVGL 8 (UI) + GT911 (tactil por Wire).
// La capa de red/API es la misma del panel CYD (esp32-vm-switcher).
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <lvgl.h>
#include "lgfx_7c.h"
#include "secrets.h"

// ================= Hardware =================
static const uint16_t SCR_W = 800, SCR_H = 480;
static const int PIN_I2C_SDA = 47, PIN_I2C_SCL = 48, PIN_TOUCH_INT = 4;
static const uint8_t IO_EXP_ADDR = 0x24, GT911_ADDR = 0x5D;
static const uint8_t IOEXP_TOUCH_RST = 1, IOEXP_BACKLIGHT = 2;

LGFX_7C lcd;

// ---- Expansor I2C 0x24 (backlight/reset) ----
static uint8_t g_ioValue = 0xFF;
static void ioExpWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(IO_EXP_ADDR); Wire.write(reg); Wire.write(val); Wire.endTransmission();
}
static void ioExpInit() { ioExpWrite(0x02, 0xFF); g_ioValue = 0xFF; ioExpWrite(0x03, g_ioValue); }
static void ioExpSet(uint8_t pin, bool high) {
  if (high) g_ioValue |= (1 << pin); else g_ioValue &= ~(1 << pin);
  ioExpWrite(0x03, g_ioValue);
}

// ---- GT911 tactil ----
static void gt911Reset() {
  pinMode(PIN_TOUCH_INT, OUTPUT); digitalWrite(PIN_TOUCH_INT, LOW);
  ioExpSet(IOEXP_TOUCH_RST, false); delay(12);
  digitalWrite(PIN_TOUCH_INT, LOW); delay(2);
  ioExpSet(IOEXP_TOUCH_RST, true);  delay(6);
  digitalWrite(PIN_TOUCH_INT, LOW); delay(55);
  pinMode(PIN_TOUCH_INT, INPUT);    delay(50);
}
static bool gt911ReadReg(uint16_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(GT911_ADDR); Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)GT911_ADDR, (int)len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}
static void gt911WriteReg(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(GT911_ADDR); Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  Wire.write(val); Wire.endTransmission();
}
static bool gt911GetPoint(uint16_t *x, uint16_t *y) {
  uint8_t status;
  if (!gt911ReadReg(0x814E, &status, 1)) return false;
  bool pressed = false;
  if (status & 0x80) {
    if ((status & 0x0F) > 0) {
      uint8_t d[8];
      if (gt911ReadReg(0x8150, d, 8)) {
        *x = (uint16_t)d[1] << 8 | d[0];
        *y = (uint16_t)d[3] << 8 | d[2];
        pressed = true;
      }
    }
    gt911WriteReg(0x814E, 0);
  }
  return pressed;
}

// ================= LVGL glue =================
static lv_disp_draw_buf_t draw_buf;
static const uint32_t BUF_LINES = 40;
static void disp_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
  uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  lcd.pushImageDMA(area->x1, area->y1, w, h, (uint16_t *)px);
  lv_disp_flush_ready(drv);
}
// Backlight / apagado por inactividad del host (igual que el panel USB y el CYD)
bool blOn = true;                       // estado actual del backlight
unsigned long lastActivityMs = 0;       // ultimo toque en el panel
#define BL_TOUCH_GRACE_MS 120000UL      // tras tocar, mantener 2 min encendido aunque el host este idle
static void setBacklight(bool on) {
  if (on == blOn) return;
  blOn = on;
  ioExpSet(IOEXP_BACKLIGHT, on);
}

static void touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  uint16_t x, y;
  if (gt911GetPoint(&x, &y)) {
    lastActivityMs = millis();
    if (!blOn) {                        // pantalla apagada por idle: el primer toque solo la enciende
      setBacklight(true);
      data->state = LV_INDEV_STATE_REL;
      return;
    }
    data->state = LV_INDEV_STATE_PR; data->point.x = x; data->point.y = y;
  } else data->state = LV_INDEV_STATE_REL;
}

// ================= Modelo de datos =================
#define MAX_MACHINES 16
struct Machine {
  int id; char kind[3]; char label[16]; char name[24]; char status[12]; bool igpu_group;
};
Machine machines[MAX_MACHINES];
int nMachines = 0, activeIgpu = -1;

struct Occ { int id; char kind[3]; char label[16]; int cpu, mem, gpu, disk; };
Occ occ[MAX_MACHINES]; int nOcc = 0;

float hostMemAvailGB = 0, hostMemTotalGB = 0, hostLoad = 0;
float hostDiskUsedGB = 0, hostDiskTotalGB = 0;
int hostGpu = 0, hostCpus = 1;
char hostName[24] = "";
bool hostIdle = false;                  // host sin teclado/raton (llega en /occupancy) -> apagar pantalla

// RTX 3080 (endpoint /gpu)
int gpuUtil = 0, gpuMemUsed = 0, gpuMemTotal = 1, gpuTemp = 0;
float gpuPower = 0.0f;
bool gpuVfio = false;

String statusLine = "Iniciando...";
bool machinesChanged = false;

// ================= Paleta =================
#define C_BG      lv_color_hex(0x000000)
#define C_BG2     lv_color_hex(0x0B1220)   // degradado de fondo de pantalla
#define C_CARD    lv_color_hex(0x1E293B)
#define C_CARD2   lv_color_hex(0x131C2E)   // fondo inferior del degradado de tarjeta
#define C_CARD_HI lv_color_hex(0x243449)
#define C_BORDER  lv_color_hex(0x334155)
#define C_ACCENT  lv_color_hex(0x38BDF8)
#define C_TEXT    lv_color_hex(0xE2E8F0)
#define C_DIM     lv_color_hex(0x94A3B8)
#define C_GREEN   lv_color_hex(0x22C55E)
#define C_RED     lv_color_hex(0xEF4444)
#define C_IGPU    lv_color_hex(0xF59E0B)
#define C_CPU     lv_color_hex(0xF87171)
#define C_MEM     lv_color_hex(0x60A5FA)
#define C_GPU     lv_color_hex(0xFBBF24)
#define C_DISK    lv_color_hex(0x34D399)
#define C_VIOLET  lv_color_hex(0xA78BFA)   // VRAM (igual que el panel USB)

// ================= Widgets persistentes =================
lv_obj_t *scr;
lv_obj_t *lbl_title, *lbl_wifi, *lbl_igpu, *lbl_status;
lv_obj_t *grid;               // contenedor con scroll de tarjetas
lv_obj_t *hostBar[3], *hostLbl[3];   // resumen host: RAM / CPU / HDD

// Panel RTX 3080 (tercio derecho) — sparklines de area con historico 5 min
#define GPU_HIST 120          // 120 puntos * 2.5 s de sondeo = 5 min
lv_obj_t *gpu_dot, *gpu_status;
lv_obj_t *gpu_utilChart, *gpu_vramChart, *gpu_tempChart;   // graficas de area
lv_obj_t *gpu_utilVal,  *gpu_vramVal,  *gpu_tempVal;        // valor grande superpuesto
lv_chart_series_t *gpu_utilSer, *gpu_vramSer, *gpu_tempSer;

struct CardUI {
  lv_obj_t *card, *lblTitle, *pill, *lblPill, *lblName, *btnAct, *lblAct, *btnSw;
  lv_obj_t *cpuChart, *cpuVal;         // sparkline hero de CPU (historico)
  lv_chart_series_t *cpuSer;
  lv_obj_t *miniBar[3], *miniLbl[3];   // MEM / GPU / DISK: barra degradada + valor
  int id;
};
CardUI cards[MAX_MACHINES];
int nCards = 0;

// ================= Forward decls =================
bool pollMachines(); bool pollOccupancy(); bool pollGpu();
bool doStart(int); bool doStop(int); bool doSwitch(int);
void connectWiFi(); void setupOTA();
int findIdx(int); int findOcc(int);
void buildUI(); void refreshMachinesUI(); void refreshHostUI(); void refreshGpuUI(); void setStatus(const String&);
static lv_color_t gpuColor(int pct);
void pumpLvgl(uint32_t ms);

// ================= Red / API (igual que el CYD) =================
String apiUrl(const String &path) { return String("http://") + API_HOST + ":" + API_PORT + path; }

bool pollMachines() {
  if (WiFi.status() != WL_CONNECTED) { statusLine = "WiFi caida"; return false; }
  HTTPClient http; http.setTimeout(5000);
  http.begin(apiUrl("/machines")); http.addHeader("X-API-Key", API_TOKEN);
  int code = http.GET();
  if (code != 200) { statusLine = String("API err ") + code; http.end(); return false; }
  String body = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) { statusLine = "JSON err"; return false; }
  activeIgpu = doc["active_igpu"].is<int>() ? doc["active_igpu"].as<int>() : -1;
  int n = 0;
  for (JsonObject m : doc["machines"].as<JsonArray>()) {
    if (n >= MAX_MACHINES) break;
    machines[n].id = m["id"].as<int>();
    const char *k = m["kind"].as<const char*>();  if (!k) k = "?";
    const char *nm = m["name"].as<const char*>(); if (!nm) nm = "?";
    const char *lb = m["label"].is<const char*>() ? m["label"].as<const char*>() : nullptr;
    const char *st = m["status"].as<const char*>(); if (!st) st = "?";
    strlcpy(machines[n].kind, k, sizeof(machines[n].kind));
    strlcpy(machines[n].name, nm, sizeof(machines[n].name));
    strlcpy(machines[n].label, lb ? lb : nm, sizeof(machines[n].label));
    strlcpy(machines[n].status, st, sizeof(machines[n].status));
    machines[n].igpu_group = m["igpu_group"].as<bool>();
    n++;
  }
  qsort(machines, n, sizeof(Machine), [](const void *a, const void *b) {
    return ((const Machine*)a)->id - ((const Machine*)b)->id;
  });
  machinesChanged = (n != nMachines);
  nMachines = n;
  return true;
}

bool _postSimple(const String &path, const String &okLabel, int vmid) {
  setStatus(okLabel + " " + vmid + "...");
  HTTPClient http; http.setTimeout(10000);
  http.begin(apiUrl(path)); http.addHeader("X-API-Key", API_TOKEN);
  http.addHeader("Content-Length", "0");
  int code = http.POST("");
  setStatus((code == 200) ? (okLabel + " " + vmid + " OK") : (okLabel + " err " + code));
  http.end();
  return code == 200;
}
bool doStart(int v)  { return _postSimple(String("/start/")  + v, "ENC", v); }
bool doStop(int v)   { return _postSimple(String("/stop/")   + v, "APG", v); }
bool doSwitch(int v) { return _postSimple(String("/switch/") + v, "Switch", v); }

bool pollOccupancy() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.setTimeout(5000);
  http.begin(apiUrl("/occupancy")); http.addHeader("X-API-Key", API_TOKEN);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  int n = 0;
  for (JsonObject m : doc["machines"].as<JsonArray>()) {
    if (n >= MAX_MACHINES) break;
    occ[n].id = m["id"].as<int>();
    const char *k = m["kind"].as<const char*>();  if (!k) k = "?";
    const char *lb = m["label"].as<const char*>(); if (!lb) lb = "?";
    strlcpy(occ[n].kind, k, sizeof(occ[n].kind));
    strlcpy(occ[n].label, lb, sizeof(occ[n].label));
    occ[n].cpu = m["cpu"].as<float>(); occ[n].mem = m["mem"].as<float>();
    occ[n].gpu = m["gpu"].as<int>();   occ[n].disk = m["disk"].as<float>();
    n++;
  }
  nOcc = n;
  hostMemAvailGB = doc["host"]["mem_avail_mb"].as<float>() / 1024.0f;
  hostMemTotalGB = doc["host"]["mem_total_mb"].as<float>() / 1024.0f;
  hostLoad = doc["host"]["load1"].as<float>();
  hostCpus = doc["host"]["cpus"].as<int>(); if (hostCpus < 1) hostCpus = 1;
  const char *hn = doc["host"]["name"] | "";
  if (hn && hn[0]) { strncpy(hostName, hn, sizeof(hostName) - 1); hostName[sizeof(hostName) - 1] = 0; }
  hostDiskUsedGB = doc["host"]["disk_used_gb"].as<float>();
  hostDiskTotalGB = doc["host"]["disk_total_gb"].as<float>();
  hostGpu = doc["host"]["gpu"].as<int>();
  hostIdle = doc["host"]["idle"] | false;   // monitores del host apagados por inactividad
  return true;
}

bool pollGpu() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http; http.setTimeout(5000);
  http.begin(apiUrl("/gpu")); http.addHeader("X-API-Key", API_TOKEN);
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject g = doc["gpu"];
  gpuVfio     = g["vfio"].as<bool>();
  gpuUtil     = g["util"].as<int>();
  gpuMemUsed  = g["mem_used"].as<int>();
  gpuMemTotal = g["mem_total"].as<int>(); if (gpuMemTotal < 1) gpuMemTotal = 1;
  gpuTemp     = g["temp"].as<int>();
  gpuPower    = g["power"].as<float>();
  return true;
}

int findIdx(int v) { for (int i = 0; i < nMachines; i++) if (machines[i].id == v) return i; return -1; }
int findOcc(int v) { for (int i = 0; i < nOcc; i++) if (occ[i].id == v) return i; return -1; }

// ================= WiFi / OTA =================
void connectWiFi() {
  WiFi.mode(WIFI_STA); WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  setStatus("WiFi conectando...");
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 60) { pumpLvgl(500); retries++; }
  setStatus(WiFi.status() == WL_CONNECTED ? "WiFi OK" : "WiFi FAIL");   // la IP ya sale en la cabecera
}
void setupOTA() {
  ArduinoOTA.setHostname("esp32-s3-vm-switcher");
#ifdef OTA_PASSWORD
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif
  ArduinoOTA.onStart([]() { setStatus("OTA inicio..."); });
  ArduinoOTA.onEnd([]()   { setStatus("OTA done"); });
  ArduinoOTA.onError([](ota_error_t e) { setStatus(String("OTA err ") + (int)e); });
  ArduinoOTA.begin();
}

// ================= UI =================
static void act_event(lv_event_t *e) {
  int action = (int)(intptr_t)lv_event_get_user_data(e);       // 0=on 1=off 2=switch 3=toggle
  lv_obj_t *btn = lv_event_get_target(e);
  int id = (int)(intptr_t)lv_obj_get_user_data(btn);
  if (action == 3) {                       // boton unico: alterna segun estado actual
    int idx = findIdx(id);
    bool running = (idx >= 0 && strcmp(machines[idx].status, "running") == 0);
    if (running) doStop(id); else doStart(id);
  }
  else if (action == 0) doStart(id);
  else if (action == 1) doStop(id);
  else doSwitch(id);
  pollMachines(); refreshMachinesUI();
}

static void header_event(lv_event_t *e) {  // tocar la cabecera = refrescar ya
  (void)e;
  pollMachines(); pollOccupancy(); refreshMachinesUI(); refreshHostUI();
}

// ---- Estilo compartido: tarjeta con degradado vertical + sombra suave ----
static void styleCardLike(lv_obj_t *o) {
  lv_obj_set_style_bg_color(o, C_CARD, 0);
  lv_obj_set_style_bg_grad_color(o, C_CARD2, 0);
  lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_border_color(o, C_BORDER, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, 14, 0);
  lv_obj_set_style_shadow_width(o, 14, 0);
  lv_obj_set_style_shadow_color(o, lv_color_hex(0x000000), 0);
  lv_obj_set_style_shadow_opa(o, LV_OPA_50, 0);
  lv_obj_set_style_shadow_ofs_y(o, 5, 0);
}

// ---- Animaciones ----
static void anim_opa_cb(void *o, int32_t v)  { lv_obj_set_style_bg_opa((lv_obj_t *)o, v, 0); }
static void anim_zoom_cb(void *o, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t *)o, v, 0); }
static void anim_bopa_cb(void *o, int32_t v) { lv_obj_set_style_border_opa((lv_obj_t *)o, v, 0); }

// punto "en vivo": circulo solido + anillo que se expande y desvanece (ping)
static lv_obj_t *makePingDot(lv_obj_t *parent, lv_color_t col, lv_align_t align, int ox, int oy) {
  // anillo (detras): solo borde, escala y desvanece en bucle
  lv_obj_t *ring = lv_obj_create(parent);
  lv_obj_remove_style_all(ring);
  lv_obj_set_size(ring, 14, 14);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(ring, 2, 0);
  lv_obj_set_style_border_color(ring, col, 0);
  lv_obj_set_style_transform_pivot_x(ring, 7, 0);
  lv_obj_set_style_transform_pivot_y(ring, 7, 0);
  lv_obj_align(ring, align, ox, oy);

  lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, ring);
  lv_anim_set_exec_cb(&a, anim_zoom_cb);
  lv_anim_set_values(&a, 256, 768);        // 100% -> 300%
  lv_anim_set_time(&a, 1600);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
  lv_anim_t b; lv_anim_init(&b); lv_anim_set_var(&b, ring);
  lv_anim_set_exec_cb(&b, anim_bopa_cb);
  lv_anim_set_values(&b, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_time(&b, 1600);
  lv_anim_set_repeat_count(&b, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&b);

  // punto solido (delante)
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_remove_style_all(dot);
  lv_obj_set_size(dot, 12, 12);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(dot, col, 0);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  lv_obj_align(dot, align, ox, oy);
  return dot;
}

static lv_obj_t *makeButton(lv_obj_t *parent, const char *txt, lv_color_t col, int action) {
  lv_obj_t *b = lv_btn_create(parent);
  lv_obj_set_height(b, 36);
  lv_obj_set_flex_grow(b, 1);
  lv_obj_set_style_bg_color(b, col, 0);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_add_event_cb(b, act_event, LV_EVENT_CLICKED, (void *)(intptr_t)action);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
  lv_obj_center(l);
  return b;
}

// barra horizontal con degradado (color -> mas claro), estilo vistoso
static void styleGradBar(lv_obj_t *bar, lv_color_t col) {
  lv_obj_set_style_bg_color(bar, C_BG, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
  lv_obj_set_style_anim_time(bar, 500, LV_PART_MAIN);   // transicion suave del valor
  lv_obj_set_style_bg_color(bar, col, LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_color(bar, lv_color_lighten(col, 110), LV_PART_INDICATOR);
  lv_obj_set_style_bg_grad_dir(bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
}

static void makeCard(int i) {
  CardUI &c = cards[i];
  c.card = lv_obj_create(grid);
  lv_obj_set_size(c.card, 252, 172);
  styleCardLike(c.card);
  lv_obj_set_style_pad_all(c.card, 8, 0);
  lv_obj_clear_flag(c.card, LV_OBJ_FLAG_SCROLLABLE);

  c.lblTitle = lv_label_create(c.card);
  lv_obj_set_style_text_font(c.lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(c.lblTitle, C_ACCENT, 0);
  lv_obj_align(c.lblTitle, LV_ALIGN_TOP_LEFT, 0, 0);

  c.pill = lv_obj_create(c.card);
  lv_obj_set_size(c.pill, 62, 22);
  lv_obj_set_style_radius(c.pill, 11, 0);
  lv_obj_set_style_border_width(c.pill, 0, 0);
  lv_obj_set_style_pad_all(c.pill, 0, 0);
  lv_obj_clear_flag(c.pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(c.pill, LV_ALIGN_TOP_RIGHT, 0, -2);
  c.lblPill = lv_label_create(c.pill);
  lv_obj_set_style_text_font(c.lblPill, &lv_font_montserrat_14, 0);
  lv_obj_center(c.lblPill);

  c.lblName = lv_label_create(c.card);
  lv_obj_set_style_text_font(c.lblName, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(c.lblName, C_TEXT, 0);
  lv_label_set_long_mode(c.lblName, LV_LABEL_LONG_DOT);
  lv_obj_set_width(c.lblName, 170);
  lv_obj_align(c.lblName, LV_ALIGN_TOP_LEFT, 0, 26);

  // ---- Sparkline hero de CPU (area con degradado, historico) ----
  c.cpuChart = lv_chart_create(c.card);
  lv_obj_set_size(c.cpuChart, 236, 40);
  lv_obj_align(c.cpuChart, LV_ALIGN_TOP_LEFT, 0, 48);
  lv_chart_set_type(c.cpuChart, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(c.cpuChart, GPU_HIST);
  lv_chart_set_range(c.cpuChart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_div_line_count(c.cpuChart, 0, 0);
  lv_chart_set_update_mode(c.cpuChart, LV_CHART_UPDATE_MODE_SHIFT);
  lv_obj_set_style_bg_color(c.cpuChart, C_BG, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(c.cpuChart, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_border_width(c.cpuChart, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(c.cpuChart, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_all(c.cpuChart, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(c.cpuChart, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(c.cpuChart, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(c.cpuChart, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(c.cpuChart, C_CPU, LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_color(c.cpuChart, C_BG, LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_dir(c.cpuChart, LV_GRAD_DIR_VER, LV_PART_ITEMS);
  c.cpuSer = lv_chart_add_series(c.cpuChart, C_CPU, LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_value(c.cpuChart, c.cpuSer, 0);

  c.cpuVal = lv_label_create(c.card);
  lv_obj_set_style_text_font(c.cpuVal, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(c.cpuVal, C_TEXT, 0);
  lv_obj_set_style_bg_color(c.cpuVal, C_BG, 0);
  lv_obj_set_style_bg_opa(c.cpuVal, LV_OPA_50, 0);
  lv_obj_set_style_pad_hor(c.cpuVal, 3, 0);
  lv_obj_set_style_radius(c.cpuVal, 3, 0);
  lv_label_set_text(c.cpuVal, "CPU --%");
  lv_obj_align(c.cpuVal, LV_ALIGN_TOP_LEFT, 4, 52);

  // ---- 3 mini-barras degradadas: MEM / GPU / DISK, con valor ----
  const char *ml[3] = {"M", "G", "D"};
  lv_color_t mc[3]  = {C_MEM, C_GPU, C_DISK};
  for (int k = 0; k < 3; k++) {
    int x = k * 79;
    c.miniLbl[k] = lv_label_create(c.card);
    lv_obj_set_style_text_font(c.miniLbl[k], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c.miniLbl[k], mc[k], 0);
    lv_label_set_text_fmt(c.miniLbl[k], "%s --", ml[k]);
    lv_obj_align(c.miniLbl[k], LV_ALIGN_TOP_LEFT, x + 2, 92);

    c.miniBar[k] = lv_bar_create(c.card);
    lv_obj_set_size(c.miniBar[k], 72, 8);
    lv_obj_align(c.miniBar[k], LV_ALIGN_TOP_LEFT, x + 2, 110);
    lv_bar_set_range(c.miniBar[k], 0, 100);
    styleGradBar(c.miniBar[k], mc[k]);
  }

  // ---- Boton unico (ON/OFF segun estado) + switch solo para grupo iGPU ----
  lv_obj_t *row = lv_obj_create(c.card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 236, 34);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 6, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  c.btnAct = lv_btn_create(row);
  lv_obj_set_height(c.btnAct, 34);
  lv_obj_set_flex_grow(c.btnAct, 1);
  lv_obj_set_style_radius(c.btnAct, 8, 0);
  lv_obj_set_style_shadow_width(c.btnAct, 0, 0);
  lv_obj_add_event_cb(c.btnAct, act_event, LV_EVENT_CLICKED, (void *)(intptr_t)3);  // 3=toggle
  c.lblAct = lv_label_create(c.btnAct);
  lv_obj_set_style_text_font(c.lblAct, &lv_font_montserrat_16, 0);
  lv_obj_center(c.lblAct);

  c.btnSw = makeButton(row, LV_SYMBOL_SHUFFLE, C_IGPU, 2);
  lv_obj_set_flex_grow(c.btnSw, 0);
  lv_obj_set_width(c.btnSw, 48);
}

// recolorea las barras (area) de una grafica segun el nivel
static void setChartColor(lv_obj_t *chart, lv_chart_series_t *ser, lv_color_t col) {
  lv_chart_set_series_color(chart, ser, col);
  lv_obj_set_style_bg_color(chart, col, LV_PART_ITEMS);
}

// crea un bloque de metrica GPU: etiqueta + sparkline de area (historico 5 min,
// barras finas contiguas con degradado vertical = area rellena) + valor grande
// superpuesto arriba-derecha. Estilo inspirado en el dashboard del vram-broker.
static void makeGpuChart(lv_obj_t *p, lv_obj_t **chart, lv_chart_series_t **ser,
                         lv_obj_t **val, const char *lbl, lv_color_t col, int y, int h) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(l, C_DIM, 0);
  lv_label_set_text(l, lbl);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 2, y);

  *chart = lv_chart_create(p);
  lv_obj_set_size(*chart, 232, h);
  lv_obj_align(*chart, LV_ALIGN_TOP_LEFT, 0, y + 18);
  lv_chart_set_type(*chart, LV_CHART_TYPE_BAR);
  lv_chart_set_point_count(*chart, GPU_HIST);
  lv_chart_set_range(*chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  lv_chart_set_div_line_count(*chart, 3, 0);
  lv_chart_set_update_mode(*chart, LV_CHART_UPDATE_MODE_SHIFT);
  // contenedor: fondo sutil, esquinas redondeadas, lineas de rejilla tenues
  lv_obj_set_style_bg_color(*chart, C_BG, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(*chart, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_border_width(*chart, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(*chart, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_all(*chart, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(*chart, 0, LV_PART_MAIN);
  lv_obj_set_style_line_color(*chart, C_BORDER, LV_PART_MAIN);
  lv_obj_set_style_line_opa(*chart, LV_OPA_20, LV_PART_MAIN);
  // barras (area): degradado vertical color -> fondo, sin hueco entre ellas
  lv_obj_set_style_radius(*chart, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(*chart, LV_OPA_COVER, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(*chart, col, LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_color(*chart, C_BG, LV_PART_ITEMS);
  lv_obj_set_style_bg_grad_dir(*chart, LV_GRAD_DIR_VER, LV_PART_ITEMS);
  *ser = lv_chart_add_series(*chart, col, LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_value(*chart, *ser, 0);

  *val = lv_label_create(p);
  lv_obj_set_style_text_font(*val, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(*val, C_TEXT, 0);
  lv_obj_set_style_bg_color(*val, C_BG, 0);
  lv_obj_set_style_bg_opa(*val, LV_OPA_60, 0);
  lv_obj_set_style_pad_hor(*val, 4, 0);
  lv_obj_set_style_radius(*val, 4, 0);
  lv_label_set_text(*val, "--");
  lv_obj_align(*val, LV_ALIGN_TOP_RIGHT, -6, y + 22);
}

void buildUI() {
  scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG2, 0);
  lv_obj_set_style_bg_grad_color(scr, C_BG, 0);
  lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ---- Cabecera ----
  lv_obj_t *hdr = lv_obj_create(scr);
  lv_obj_set_size(hdr, 800, 52);
  lv_obj_set_pos(hdr, 0, 0);
  lv_obj_set_style_bg_color(hdr, C_BG, 0);
  lv_obj_set_style_border_width(hdr, 0, 0);
  lv_obj_set_style_pad_all(hdr, 8, 0);
  lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hdr, header_event, LV_EVENT_CLICKED, NULL);

  makePingDot(hdr, C_GREEN, LV_ALIGN_LEFT_MID, 2, 0);   // indicador "en vivo"

  lbl_title = lv_label_create(hdr);
  lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(lbl_title, C_TEXT, 0);
  lv_label_set_text(lbl_title, "HOMELAB");
  lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 22, 0);

  lbl_wifi = lv_label_create(hdr);
  lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_wifi, C_DIM, 0);
  lv_label_set_text(lbl_wifi, "WiFi...");
  lv_obj_align(lbl_wifi, LV_ALIGN_CENTER, 0, 0);

  lbl_igpu = lv_label_create(hdr);
  lv_obj_set_style_text_font(lbl_igpu, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lbl_igpu, C_DIM, 0);
  lv_label_set_text(lbl_igpu, "iGPU:-");
  lv_obj_align(lbl_igpu, LV_ALIGN_RIGHT_MID, 0, 0);

  // ---- 2/3 izquierdo: rejilla de maquinas con scroll ----
  grid = lv_obj_create(scr);
  lv_obj_set_pos(grid, 4, 54);
  lv_obj_set_size(grid, 528, 352);
  lv_obj_set_style_bg_color(grid, C_BG, 0);
  lv_obj_set_style_border_width(grid, 0, 0);
  lv_obj_set_style_pad_all(grid, 6, 0);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(grid, 8, 0);
  lv_obj_set_style_pad_column(grid, 8, 0);
  for (int i = 0; i < MAX_MACHINES; i++) { makeCard(i); lv_obj_add_flag(cards[i].card, LV_OBJ_FLAG_HIDDEN); }

  // ---- Resumen del host (abajo izquierda): RAM / CPU / HDD ----
  lv_obj_t *foot = lv_obj_create(scr);
  lv_obj_set_size(foot, 528, 66);
  lv_obj_set_pos(foot, 4, 408);
  styleCardLike(foot);
  lv_obj_set_style_pad_all(foot, 8, 0);
  lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);

  lbl_status = lv_label_create(foot);
  lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(lbl_status, C_DIM, 0);
  lv_obj_align(lbl_status, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_label_set_text(lbl_status, statusLine.c_str());

  lv_color_t cols[3] = {C_MEM, C_CPU, C_DISK};
  for (int i = 0; i < 3; i++) {
    int bx = i * 172;
    hostLbl[i] = lv_label_create(foot);
    lv_obj_set_style_text_font(hostLbl[i], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hostLbl[i], C_DIM, 0);
    lv_label_set_text(hostLbl[i], "--");
    lv_obj_align(hostLbl[i], LV_ALIGN_TOP_LEFT, bx, 0);

    hostBar[i] = lv_bar_create(foot);
    lv_obj_set_size(hostBar[i], 158, 12);
    lv_obj_align(hostBar[i], LV_ALIGN_TOP_LEFT, bx, 26);
    lv_bar_set_range(hostBar[i], 0, 100);
    styleGradBar(hostBar[i], cols[i]);
  }

  // ---- 1/3 derecho: panel RTX 3080 ----
  lv_obj_t *gp = lv_obj_create(scr);
  lv_obj_set_pos(gp, 540, 54);
  lv_obj_set_size(gp, 256, 420);
  styleCardLike(gp);
  lv_obj_set_style_pad_all(gp, 12, 0);
  lv_obj_clear_flag(gp, LV_OBJ_FLAG_SCROLLABLE);

  gpu_dot = makePingDot(gp, C_GREEN, LV_ALIGN_TOP_LEFT, 0, 6);   // "en vivo" con anillo ping

  lv_obj_t *gt = lv_label_create(gp);
  lv_obj_set_style_text_font(gt, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(gt, C_TEXT, 0);
  lv_label_set_text(gt, "RTX 3080");
  lv_obj_align(gt, LV_ALIGN_TOP_LEFT, 20, 0);

  // 3 sparklines de area con historico de 5 min (GPU% / VRAM / TEMP)
  makeGpuChart(gp, &gpu_utilChart, &gpu_utilSer, &gpu_utilVal, "GPU %", C_ACCENT, 30,  88);
  makeGpuChart(gp, &gpu_vramChart, &gpu_vramSer, &gpu_vramVal, "VRAM",  C_VIOLET, 144, 88);
  makeGpuChart(gp, &gpu_tempChart, &gpu_tempSer, &gpu_tempVal, "TEMP",  C_GPU,    258, 88);

  gpu_status = lv_label_create(gp);
  lv_obj_set_style_text_font(gpu_status, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(gpu_status, C_DIM, 0);
  lv_label_set_text(gpu_status, "--");
  lv_obj_align(gpu_status, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

void setStatus(const String &s) {
  statusLine = s;
  if (lbl_status) { lv_label_set_text(lbl_status, s.c_str()); }
}

void refreshMachinesUI() {
  // Cabecera
  if (WiFi.status() == WL_CONNECTED) {
    lv_label_set_text(lbl_wifi, (WiFi.localIP().toString() + "  RSSI " + WiFi.RSSI()).c_str());
    lv_obj_set_style_text_color(lbl_wifi, C_DIM, 0);
  } else {
    lv_label_set_text(lbl_wifi, "WiFi off");
    lv_obj_set_style_text_color(lbl_wifi, C_RED, 0);
  }
  if (hostName[0]) lv_label_set_text_fmt(lbl_title, "HOMELAB : %s", hostName);
  if (activeIgpu > 0) {
    lv_label_set_text_fmt(lbl_igpu, "iGPU:%d", activeIgpu);
    lv_obj_set_style_text_color(lbl_igpu, C_IGPU, 0);
  } else {
    lv_label_set_text(lbl_igpu, "iGPU:-");
    lv_obj_set_style_text_color(lbl_igpu, C_DIM, 0);
  }

  // Tarjetas
  for (int i = 0; i < MAX_MACHINES; i++) {
    CardUI &c = cards[i];
    if (i >= nMachines) { lv_obj_add_flag(c.card, LV_OBJ_FLAG_HIDDEN); continue; }
    Machine &m = machines[i];
    c.id = m.id;
    lv_obj_clear_flag(c.card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(c.btnAct, (void *)(intptr_t)m.id);
    lv_obj_set_user_data(c.btnSw, (void *)(intptr_t)m.id);

    lv_label_set_text_fmt(c.lblTitle, "%s %d", m.kind, m.id);
    lv_label_set_text(c.lblName, m.label);

    bool running = (strcmp(m.status, "running") == 0);
    bool stopped = (strcmp(m.status, "stopped") == 0);
    lv_obj_set_style_bg_color(c.pill, running ? C_GREEN : (stopped ? C_RED : C_DIM), 0);
    lv_label_set_text(c.lblPill, running ? "ON" : (stopped ? "OFF" : m.status));
    lv_obj_set_style_text_color(c.lblPill, lv_color_hex(0x0F172A), 0);

    // borde acento si esta en el grupo iGPU / si es la iGPU activa
    if (m.id == activeIgpu) { lv_obj_set_style_border_color(c.card, C_IGPU, 0); lv_obj_set_style_border_width(c.card, 2, 0); }
    else if (m.igpu_group)  { lv_obj_set_style_border_color(c.card, C_IGPU, 0); lv_obj_set_style_border_width(c.card, 1, 0); }
    else                    { lv_obj_set_style_border_color(c.card, C_BORDER, 0); lv_obj_set_style_border_width(c.card, 1, 0); }

    // boton UNICO: ON (verde) si apagada, OFF (rojo) si encendida
    if (running)      { lv_label_set_text(c.lblAct, "OFF"); lv_obj_set_style_bg_color(c.btnAct, C_RED, 0); }
    else if (stopped) { lv_label_set_text(c.lblAct, "ON");  lv_obj_set_style_bg_color(c.btnAct, C_GREEN, 0); }
    else              { lv_label_set_text(c.lblAct, m.status); lv_obj_set_style_bg_color(c.btnAct, C_DIM, 0); }
    // boton switch solo para el grupo iGPU
    if (m.igpu_group) lv_obj_clear_flag(c.btnSw, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_add_flag(c.btnSw, LV_OBJ_FLAG_HIDDEN);

    // graficos: sparkline CPU (hero, historico) + mini-barras MEM/GPU/DISK con valor
    int oi = findOcc(m.id);
    bool hasOcc = (running && oi >= 0);
    int cpu = hasOcc ? occ[oi].cpu : 0; if (cpu < 0) cpu = 0; if (cpu > 100) cpu = 100;
    lv_chart_set_next_value(c.cpuChart, c.cpuSer, cpu);
    setChartColor(c.cpuChart, c.cpuSer, gpuColor(cpu));
    lv_label_set_text_fmt(c.cpuVal, "CPU %d%%", cpu);
    lv_obj_set_style_text_color(c.cpuVal, gpuColor(cpu), 0);

    int mv[3] = { hasOcc ? occ[oi].mem : 0, hasOcc ? occ[oi].gpu : 0, hasOcc ? occ[oi].disk : 0 };
    const char *ml[3] = {"M", "G", "D"};
    for (int k = 0; k < 3; k++) {
      int v = mv[k]; if (v < 0) v = 0; if (v > 100) v = 100;
      lv_bar_set_value(c.miniBar[k], v, LV_ANIM_ON);
      lv_label_set_text_fmt(c.miniLbl[k], "%s %d%%", ml[k], v);
    }
  }
}

void refreshHostUI() {
  int memUsed = (hostMemTotalGB > 0.1f) ? (int)((hostMemTotalGB - hostMemAvailGB) / hostMemTotalGB * 100 + 0.5f) : 0;
  int loadPct = (int)(hostLoad / hostCpus * 100 + 0.5f); if (loadPct > 100) loadPct = 100;
  int diskPct = (hostDiskTotalGB > 0.1f) ? (int)(hostDiskUsedGB / hostDiskTotalGB * 100 + 0.5f) : 0;
  int vals[3] = {memUsed, loadPct, diskPct};
  for (int i = 0; i < 3; i++) lv_bar_set_value(hostBar[i], vals[i], LV_ANIM_ON);
  // OJO: lv_label_set_text_fmt NO soporta %f (LVGL sin float) -> pre-formatear con snprintf
  char b[24];
  snprintf(b, sizeof(b), "RAM %.0f/%.0fG", hostMemTotalGB - hostMemAvailGB, hostMemTotalGB);
  lv_label_set_text(hostLbl[0], b);
  lv_label_set_text_fmt(hostLbl[1], "CPU %d%%", loadPct);
  // HDD solo GB usados (la barra ya muestra el %) para no chocar con el estado
  snprintf(b, sizeof(b), "HDD %.0fG", hostDiskUsedGB);
  lv_label_set_text(hostLbl[2], b);
}

static lv_color_t gpuColor(int pct) {   // igual que color_for() del panel USB
  if (pct >= 85) return C_RED;
  if (pct >= 55) return C_GPU;
  return C_GREEN;
}

void refreshGpuUI() {
  char b[32];
  if (gpuVfio) {
    lv_obj_set_style_bg_color(gpu_dot, C_RED, 0);
    lv_label_set_text(gpu_utilVal, "VFIO");
    lv_obj_set_style_text_color(gpu_utilVal, C_IGPU, 0);
    lv_label_set_text(gpu_vramVal, "VM");
    lv_label_set_text(gpu_tempVal, "-");
    lv_label_set_text(gpu_status, "3080 en passthrough (VM)");
    return;
  }
  lv_obj_set_style_bg_color(gpu_dot, C_GREEN, 0);

  int memPct  = (int)((float)gpuMemUsed * 100 / gpuMemTotal + 0.5f);
  int tempPct = (int)(gpuTemp * 100 / 90); if (tempPct > 100) tempPct = 100; if (tempPct < 0) tempPct = 0;

  // empujar muestra al historico (la grafica desplaza y conserva 5 min)
  lv_chart_set_next_value(gpu_utilChart, gpu_utilSer, gpuUtil);
  lv_chart_set_next_value(gpu_vramChart, gpu_vramSer, memPct);
  lv_chart_set_next_value(gpu_tempChart, gpu_tempSer, tempPct);

  // GPU%: valor grande + color por nivel (verde/ambar/rojo) tambien en el area
  lv_label_set_text_fmt(gpu_utilVal, "%d%%", gpuUtil);
  lv_obj_set_style_text_color(gpu_utilVal, gpuColor(gpuUtil), 0);
  setChartColor(gpu_utilChart, gpu_utilSer, gpuColor(gpuUtil));

  // VRAM: GB usados (grande), area violeta fija
  snprintf(b, sizeof(b), "%.1fG", gpuMemUsed / 1024.0f);
  lv_label_set_text(gpu_vramVal, b);

  // TEMP: grados (grande) + color por nivel
  lv_label_set_text_fmt(gpu_tempVal, "%dC", gpuTemp);
  lv_obj_set_style_text_color(gpu_tempVal, gpuColor(tempPct), 0);
  setChartColor(gpu_tempChart, gpu_tempSer, gpuColor(tempPct));

  // resumen abajo: VRAM absoluta + potencia (ya no tienen barra propia)
  snprintf(b, sizeof(b), "%.1f/%.0fG  %.0fW", gpuMemUsed / 1024.0f, gpuMemTotal / 1024.0f, gpuPower);
  lv_label_set_text(gpu_status, b);
}

// Corre LVGL durante ~ms (para no congelar la UI en esperas)
void pumpLvgl(uint32_t ms) {
  uint32_t t0 = millis();
  do { lv_timer_handler(); delay(5); } while (millis() - t0 < ms);
}

// ================= Setup / loop =================
void setup() {
  Serial.begin(115200);
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
  ioExpInit();
  ioExpSet(IOEXP_BACKLIGHT, true);
  gt911Reset();

  lcd.init();
  lcd.setSwapBytes(false);  // el swap RGB565 lo hace LVGL (LV_COLOR_16_SWAP=1); aqui NO, para no doble-swap

  lv_init();
  size_t bytes = SCR_W * BUF_LINES * sizeof(lv_color_t);
  lv_color_t *b1 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  lv_color_t *b2 = (lv_color_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
  lv_disp_draw_buf_init(&draw_buf, b1, b2, SCR_W * BUF_LINES);
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCR_W; disp_drv.ver_res = SCR_H;
  disp_drv.flush_cb = disp_flush; disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER; indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  buildUI();
  pumpLvgl(50);

  connectWiFi();
  setupOTA();
  pollMachines();
  pollOccupancy();
  pollGpu();
  refreshMachinesUI();
  refreshHostUI();
  refreshGpuUI();
}

unsigned long lastPoll = 0;
void loop() {
  ArduinoOTA.handle();
  lv_timer_handler();
  if (millis() - lastPoll > 2500) {
    lastPoll = millis();
    bool ok = pollMachines();
    pollOccupancy();
    pollGpu();
    if (ok) { refreshMachinesUI(); refreshHostUI(); refreshGpuUI(); }
    else { refreshGpuUI(); setStatus(statusLine); }
    // Apagar la pantalla si el host lleva idle (monitores off) salvo gracia tras tocar
    setBacklight(!hostIdle || (millis() - lastActivityMs < BL_TOUCH_GRACE_MS));
  }
  delay(5);
}
