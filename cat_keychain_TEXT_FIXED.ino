#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "esp_sleep.h"

// ---------- PINS ----------
#define SDA_PIN 5
#define SCL_PIN 6
#define BTN_PIN 4                 // button between GPIO4 and GND

// ---------- WIFI ----------
#define WIFI_SSID "iphone"
#define WIFI_PASS "12345678"
#define AP_SSID   "CatKeychain"   // fallback hotspot made by the keychain
#define AP_PASS   "12345678"      // min 8 characters
#define HOSTNAME  "catkey"        // http://catkey.local

// ---------- SETTINGS ----------
#define SCENE_MS      3000UL      // every animation lasts 3 seconds
#define IDLE_SLEEP_MS 0UL    // deep sleep after this long with no button/phone activity (0 = never)

// ---------- DISPLAY (1.3" = SH1106, address 0x3D) ----------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

WebServer server(80);

// ---------- EYE MOODS ----------
enum Mood { M_NORMAL, M_CUTE, M_ANGRY, M_SWEET, M_CAT, M_LOVE, M_SURPRISED,
            M_SLEEPY, M_SAD, M_SUSPICIOUS, M_COUNT };

struct Face {
  float w, h;       // eye width / height
  float rx, ry;     // pupil radius x / y
  float slant;      // >0 angry lid, <0 sad lid (px)
  float lidTop;     // 0..1 top lid cover
  float happy;      // 0..1 ^_^ shape
  float blush;      // 0..1 cheek lines
  float sparkle;    // 0..1 eye highlights
};

const Face PRESET[M_COUNT] = {
  //  w   h   rx  ry  slant lid   happy blush sparkle
  { 36, 40,  9, 11,   0,  0.00f, 0, 0, 0 },   // NORMAL
  { 38, 44, 14, 17,   0,  0.00f, 0, 0, 1 },   // CUTE
  { 38, 34,  7,  8,  18,  0.00f, 0, 0, 0 },   // ANGRY
  { 36, 36, 11, 12,   0,  0.00f, 1, 1, 0 },   // SWEET
  { 40, 44,  3, 19,   0,  0.00f, 0, 0, 0 },   // CAT (slit pupils)
  { 36, 40,  9, 11,   0,  0.00f, 0, 0, 0 },   // LOVE (hearts drawn separately)
  { 30, 46,  4,  5,   0,  0.00f, 0, 0, 0 },   // SURPRISED
  { 38, 30,  8,  9,   0,  0.55f, 0, 0, 0 },   // SLEEPY
  { 36, 40, 11, 12, -14,  0.10f, 0, 0, 1 },   // SAD
  { 40, 34,  8,  9,   0,  0.40f, 0, 0, 0 }    // SUSPICIOUS
};

// ---------- SCENES ----------
// (order matters: the web page buttons use these numbers)
enum Scene {
  S_DANCE, S_NORMAL, S_CUTE, S_ANGRY, S_SWEET, S_CAT, S_LOVE, S_SURPRISED,
  S_SLEEPY, S_WINK, S_DIZZY, S_STARS, S_SAD, S_SIDEEYE, S_ROLL, S_CATFACE,
  S_WALK, S_BALL, S_HEARTRAIN, S_MEOW, S_SLEEPCAT,
  S_MESSAGE,            // phone message (not part of the shuffle)
  SCENE_COUNT
};
const int ANIM_COUNT = S_MESSAGE;   // number of shuffled animations

Face cur = PRESET[M_NORMAL];
Mood mood = M_NORMAL;
Scene scene = S_DANCE;
unsigned long sceneStart = 0;
unsigned long sceneDur = SCENE_MS;

// shuffle bag so every animation plays once before any repeats
uint8_t bag[ANIM_COUNT];
int bagPos = ANIM_COUNT;

// look / blink
float lookX = 0, lookY = 0, tLookX = 0, tLookY = 0;
unsigned long nextLook = 0, nextBlink = 0, blinkStart = 0;
bool blinking = false;
const unsigned long BLINK_MS = 160;

// button / power
bool hold = false;            // long press = stay on the current animation
bool sleeping = false;
bool btnDown = false;
unsigned long pressStart = 0, lastActivity = 0;

// phone message
String msgText = "";
bool msgScroll = false;
bool msgPending = false;
unsigned long msgDur = 6000;
volatile bool messageActive = false;
unsigned long messageStart = 0;

// wifi state
bool serverStarted = false, staAnnounced = false, apStarted = false;
unsigned long wifiBeganAt = 0;

// =====================================================
//                    SMALL HELPERS
// =====================================================
float blinkLevel() {
  if (!blinking) return 0;
  float t = (float)(millis() - blinkStart) / BLINK_MS;
  if (t >= 1.0f) { blinking = false; return 0; }
  return sinf(t * PI);
}

void drawHeart(int cx, int cy, int r) {
  u8g2.drawDisc(cx - r, cy - r / 2, r);
  u8g2.drawDisc(cx + r, cy - r / 2, r);
  int ty = cy - r / 2 + (r * 2) / 5 + 1;
  u8g2.drawTriangle(cx - 2 * r + 1, ty, cx + 2 * r - 1, ty, cx, cy + 2 * r + r / 3);
}

void drawNote(int x, int y) {
  u8g2.drawDisc(x, y, 2);
  u8g2.drawLine(x + 2, y, x + 2, y - 7);
  u8g2.drawLine(x + 2, y - 7, x + 5, y - 5);
}

void drawStar4(int cx, int cy, int R) {
  int r = max(2, R / 4);
  u8g2.drawTriangle(cx - r, cy, cx + r, cy, cx, cy - R);
  u8g2.drawTriangle(cx - r, cy, cx + r, cy, cx, cy + R);
  u8g2.drawTriangle(cx, cy - r, cx, cy + r, cx - R, cy);
  u8g2.drawTriangle(cx, cy - r, cx, cy + r, cx + R, cy);
  u8g2.drawBox(cx - r, cy - r, 2 * r + 1, 2 * r + 1);
}

void thickLine(int x0, int y0, int x1, int y1) {
  u8g2.drawLine(x0, y0, x1, y1);
  u8g2.drawLine(x0 + 1, y0, x1 + 1, y1);
  u8g2.drawLine(x0, y0 + 1, x1, y1 + 1);
}

void ballPos(unsigned long t, int &bx, int &by) {
  bx = 64 + (int)(52 * sinf(t / 520.0f));
  by = 59 - (int)(fabsf(sinf(t / 230.0f)) * 11);
}

// =====================================================
//                    EYE ENGINE
// =====================================================
void drawEye(int cx, int cy, int dir) {   // dir: -1 = left eye, +1 = right eye
  int w = max(4, (int)cur.w), h = max(4, (int)cur.h);
  int x = cx - w / 2, y = cy - h / 2;
  int r = max(1, min(w, h) / 2 - 1);

  u8g2.setDrawColor(1);
  u8g2.drawRBox(x, y, w, h, r);

  // pupil
  int rx = max(1, (int)cur.rx), ry = max(1, (int)cur.ry);
  float rangeX = max(0.0f, w / 2.0f - rx - 3.0f);
  float rangeY = max(0.0f, h / 2.0f - ry - 3.0f);
  int px = cx + (int)(lookX * rangeX);
  int py = cy + (int)(lookY * rangeY);
  u8g2.setDrawColor(0);
  u8g2.drawFilledEllipse(px, py, rx, ry);

  // sparkle highlights
  if (cur.sparkle > 0.5f && rx > 5) {
    u8g2.setDrawColor(1);
    u8g2.drawDisc(px - rx / 3, py - ry / 3, max(2, rx / 3));
    u8g2.drawDisc(px + rx / 3, py + ry / 3, 1);
  }

  // top lid (sleepy + blink)
  float lid = max(cur.lidTop, blinkLevel());
  u8g2.setDrawColor(0);
  if (lid > 0.01f) u8g2.drawBox(x - 1, y - 1, w + 2, (int)(lid * h) + 1);

  // slanted lid: angry (slant > 0) or sad (slant < 0)
  if (fabsf(cur.slant) > 1.0f) {
    int s = (int)fabsf(cur.slant);
    int innerX = (dir < 0) ? x + w : x;
    int outerX = (dir < 0) ? x : x + w;
    int lowSide  = (cur.slant > 0) ? innerX : outerX;
    int highSide = (cur.slant > 0) ? outerX : innerX;
    u8g2.drawTriangle(highSide, y - 1, lowSide, y - 1, lowSide, y + s);
  }

  // happy ^_^ : black disc eats the lower part of the eye
  if (cur.happy > 0.02f) {
    int dy = cy + (int)(h * 0.35f) + (int)((1.0f - cur.happy) * h);
    u8g2.drawDisc(cx, dy, (int)(w * 0.65f));
  }
  u8g2.setDrawColor(1);
}

void drawEyeScene(unsigned long t) {
  int jx = (mood == M_ANGRY) ? random(-1, 2) : 0;   // angry jitter
  int cy = 24;

  if (mood == M_LOVE) {
    int r = (int)(7 * (1.0f + 0.15f * sinf(millis() / 150.0f)));
    drawHeart(32, cy + 2, r);
    drawHeart(96, cy + 2, r);
  } else {
    drawEye(32 + jx, cy, -1);
    if (scene == S_WINK) {
      bool winking = (t > 500 && t < 1300) || (t > 1700 && t < 2500);
      if (winking) { Face s = cur; cur.happy = 1; drawEye(96, cy, 1); cur = s; }
      else drawEye(96, cy, 1);
    } else {
      drawEye(96 + jx, cy, 1);
    }
  }

  // cat mouth "w"
  if (mood == M_CUTE || mood == M_SWEET || mood == M_CAT || mood == M_LOVE) {
    u8g2.setDrawColor(1);
    u8g2.drawCircle(61, 52, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    u8g2.drawCircle(67, 52, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  }

  // blush lines
  if (cur.blush > 0.5f) {
    for (int i = 0; i < 3; i++) {
      u8g2.drawLine(14 + i * 5, 58, 18 + i * 5, 52);
      u8g2.drawLine(100 + i * 5, 58, 104 + i * 5, 52);
    }
  }

  // floating Zzz
  if (mood == M_SLEEPY) {
    u8g2.setFont(u8g2_font_6x10_tr);
    int k = (t / 500) % 3;
    u8g2.drawStr(104 + k * 5, 50 - k * 8, "z");
  }

  // "!" for surprised
  if (scene == S_SURPRISED) {
    u8g2.setFont(u8g2_font_ncenB14_tr);
    u8g2.drawStr(116, 22, "!");
  }

  // falling tears for sad
  if (mood == M_SAD) {
    const int xs[4] = {24, 40, 88, 104};
    const int ph[4] = {0, 12, 7, 19};
    for (int i = 0; i < 4; i++) {
      int y = 42 + (int)((t / 30 + ph[i]) % 24);
      u8g2.drawDisc(xs[i], y, 2);
      u8g2.drawTriangle(xs[i] - 2, y, xs[i] + 2, y, xs[i], y - 4);
    }
  }
}

// =====================================================
//                 SPECIAL SCENES
// =====================================================

// ---- cute cat dancing ----
void drawDanceCat(unsigned long t) {
  float beat = t / 400.0f * PI;
  float sw = sinf(beat);
  int by = -(int)(fabsf(sw) * 5);       // whole body bounces
  u8g2.setDrawColor(1);

  // music notes floating up
  const int nxs[3] = {8, 112, 20};
  for (int k = 0; k < 3; k++) {
    int ny = 60 - (int)((t / 25 + k * 30) % 56);
    drawNote(nxs[k] + (int)(3 * sinf(t / 200.0f + k)), ny);
  }

  // tail (swishing)
  for (int i = 0; i <= 10; i++) {
    float s = i / 10.0f;
    int tx = 74 + (int)(18 * s);
    int ty = 52 + by - (int)(12 * s) + (int)(4 * sinf(beat * 1.5f + s * 4) * s);
    u8g2.drawDisc(tx, ty, (i == 10) ? 2 : 1);
  }

  // legs (alternate kicks)
  int footLx = 56 + (int)(3 * sw),  footLy = 62 - (sw > 0 ? (int)(sw * 5) : 0);
  int footRx = 72 - (int)(3 * sw),  footRy = 62 - (sw < 0 ? (int)(-sw * 5) : 0);
  u8g2.drawLine(59, 52 + by, footLx, footLy);
  u8g2.drawLine(60, 52 + by, footLx + 1, footLy);
  u8g2.drawLine(69, 52 + by, footRx, footRy);
  u8g2.drawLine(70, 52 + by, footRx + 1, footRy);
  u8g2.drawFilledEllipse(footLx, footLy, 4, 2);
  u8g2.drawFilledEllipse(footRx, footRy, 4, 2);

  // body
  u8g2.drawRBox(54, 32 + by, 20, 20, 6);
  u8g2.setDrawColor(0);
  for (int i = 0; i < 2; i++) {          // stripes
    u8g2.drawLine(54, 40 + i * 4 + by, 58, 40 + i * 4 + by);
    u8g2.drawLine(70, 40 + i * 4 + by, 74, 40 + i * 4 + by);
  }
  u8g2.setDrawColor(1);

  // arms (alternate waving)
  int hLx = 38, hLy = 34 + by - (int)(14 * sw);
  int hRx = 90, hRy = 34 + by + (int)(14 * sw);
  thickLine(55, 36 + by, hLx, hLy);
  thickLine(73, 36 + by, hRx, hRy);
  u8g2.drawDisc(hLx, hLy, 2);
  u8g2.drawDisc(hRx, hRy, 2);

  // head + ears
  u8g2.drawRBox(49, 9 + by, 30, 22, 9);
  u8g2.drawTriangle(50, 15 + by, 51, 3 + by, 60, 10 + by);
  u8g2.drawTriangle(78, 15 + by, 77, 3 + by, 68, 10 + by);
  // whiskers
  u8g2.drawLine(49, 24 + by, 41, 22 + by);
  u8g2.drawLine(49, 27 + by, 41, 28 + by);
  u8g2.drawLine(79, 24 + by, 87, 22 + by);
  u8g2.drawLine(79, 27 + by, 87, 28 + by);
  // happy ^ ^ eyes, nose, mouth
  u8g2.setDrawColor(0);
  u8g2.drawCircle(58, 22 + by, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawCircle(58, 22 + by, 2, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawCircle(70, 22 + by, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawCircle(70, 22 + by, 2, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
  u8g2.drawPixel(64, 25 + by);
  u8g2.drawCircle(62, 27 + by, 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.drawCircle(66, 27 + by, 2, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.setDrawColor(1);
}

// ---- dizzy spiral eyes ----
void drawSpiral(int cx, int cy, float phase, float dir) {
  for (float a = 0; a < 13.0f; a += 0.12f) {
    float rr = a * 1.35f;
    float ang = dir * a + phase;
    u8g2.drawDisc(cx + (int)(rr * cosf(ang)), cy + (int)(rr * sinf(ang)), 1);
  }
}

void drawDizzy(unsigned long t) {
  float ph = t / 120.0f;
  int wob = (int)(3 * sinf(t / 200.0f));
  drawSpiral(32 + wob, 24, ph, 1);
  drawSpiral(96 + wob, 24, -ph, -1);
  for (int x = 46; x < 82; x++)                         // wobbly mouth
    u8g2.drawPixel(x, 54 + (int)(2 * sinf(x * 0.5f + t / 100.0f)));
}

// ---- sparkle star eyes ----
void drawStarsScene(unsigned long t) {
  int R1 = 16 + (int)(3 * sinf(t / 130.0f));
  int R2 = 16 + (int)(3 * sinf(t / 130.0f + 1.5f));
  drawStar4(32, 24, R1);
  drawStar4(96, 24, R2);
  u8g2.drawDisc(64, 46, 5, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);   // open smile
  const int sx[8] = {10, 30, 64, 118, 100, 64, 64, 6};
  const int sy[8] = {56, 58, 10, 56, 58, 30, 58, 34};
  for (int i = 0; i < 8; i++) {
    if (((t / 150) + i) % 3 == 0) {
      u8g2.drawPixel(sx[i], sy[i]);
      u8g2.drawPixel(sx[i] - 1, sy[i]); u8g2.drawPixel(sx[i] + 1, sy[i]);
      u8g2.drawPixel(sx[i], sy[i] - 1); u8g2.drawPixel(sx[i], sy[i] + 1);
    }
  }
}

// ---- cat face with moving ears / whiskers ----
void drawCatFace(unsigned long t) {
  int ewL = (int)(2 * sinf(t / 120.0f));
  int ewR = (int)(2 * sinf(t / 120.0f + 2));
  int tw  = (int)(2 * sinf(t / 90.0f));
  u8g2.setDrawColor(1);

  // ears
  u8g2.drawTriangle(24, 30, 26, 4 + ewL, 48, 19);
  u8g2.drawTriangle(104, 30, 102, 4 + ewR, 80, 19);
  u8g2.setDrawColor(0);
  u8g2.drawTriangle(30, 24, 29, 12 + ewL, 40, 20);
  u8g2.drawTriangle(98, 24, 99, 12 + ewR, 88, 20);
  u8g2.setDrawColor(1);

  // head outline
  u8g2.drawRFrame(22, 18, 84, 42, 16);
  u8g2.drawRFrame(23, 19, 82, 40, 15);

  // eyes
  int px = (int)(lookX * 3), py = (int)(lookY * 2);
  u8g2.drawFilledEllipse(46, 36, 7, 8);
  u8g2.drawFilledEllipse(82, 36, 7, 8);
  u8g2.setDrawColor(0);
  u8g2.drawFilledEllipse(46 + px, 36 + py, 3, 6);
  u8g2.drawFilledEllipse(82 + px, 36 + py, 3, 6);
  float bl = blinkLevel();
  if (bl > 0.2f) {
    int bh = (int)(bl * 18);
    u8g2.drawBox(38, 27, 17, bh);
    u8g2.drawBox(74, 27, 17, bh);
  }
  u8g2.setDrawColor(1);

  // nose
  u8g2.drawTriangle(61, 42, 67, 42, 64, 46);

  // mouth: closed "w", opens to meow
  bool open = (t > 1000) && ((t / 450) % 2 == 1);
  if (open) {
    u8g2.drawDisc(64, 52, 3);
  } else {
    u8g2.drawCircle(61, 49, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
    u8g2.drawCircle(67, 49, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  }

  // whiskers
  u8g2.drawLine(20, 42, 4, 38 + tw);
  u8g2.drawLine(20, 46, 4, 47);
  u8g2.drawLine(20, 50, 4, 56 - tw);
  u8g2.drawLine(108, 42, 124, 38 + tw);
  u8g2.drawLine(108, 46, 124, 47);
  u8g2.drawLine(108, 50, 124, 56 - tw);
}

// ---- cat walking across the screen ----
void drawWalkCat(unsigned long t) {
  float p = t / (float)SCENE_MS;
  int x0 = -48 + (int)(p * (128 + 56));
  float ph = t / 110.0f;
  int by = (int)(sinf(ph * 2) * 1.2f);
  u8g2.setDrawColor(1);

  // moving ground dashes
  for (int i = 0; i < 128; i += 16) {
    int gx = (i - (int)(t / 6)) % 128;
    if (gx < 0) gx += 128;
    u8g2.drawHLine(gx, 60, 6);
  }

  // tail
  for (int i = 0; i < 10; i++) {
    int tx = x0 + 8 - (int)(i * 1.4f);
    int ty = 34 + by - (int)(i * 2.4f) + (int)(2 * sinf(t / 140.0f + i * 0.6f));
    u8g2.drawDisc(tx, ty, 1);
  }

  // body, head, ears
  u8g2.drawRBox(x0 + 8, 28 + by, 30, 15, 6);
  u8g2.drawDisc(x0 + 42, 28 + by, 8);
  u8g2.drawTriangle(x0 + 36, 24 + by, x0 + 37, 15 + by, x0 + 42, 21 + by);
  u8g2.drawTriangle(x0 + 43, 21 + by, x0 + 48, 15 + by, x0 + 48, 25 + by);
  u8g2.drawLine(x0 + 50, 30 + by, x0 + 57, 28 + by);   // whiskers
  u8g2.drawLine(x0 + 50, 32 + by, x0 + 57, 33 + by);

  // eye + stripes
  u8g2.setDrawColor(0);
  u8g2.drawDisc(x0 + 45, 27 + by, 1);
  for (int i = 0; i < 3; i++)
    u8g2.drawLine(x0 + 16 + i * 8, 28 + by, x0 + 18 + i * 8, 34 + by);
  u8g2.setDrawColor(1);

  // four legs (diagonal pairs)
  const int lx[4]   = {12, 18, 30, 35};
  const float off[4] = {0, PI, PI, 0};
  for (int i = 0; i < 4; i++) {
    int bx = x0 + lx[i];
    int fx = bx + (int)(5 * sinf(ph + off[i]));
    int fy = 57 - (int)(max(0.0f, cosf(ph + off[i])) * 3);
    u8g2.drawLine(bx, 42 + by, fx, fy);
    u8g2.drawLine(bx + 1, 42 + by, fx + 1, fy);
    u8g2.drawHLine(fx, fy, 3);
  }
}

// ---- eyes chase a bouncing ball (look set in updateAnimation) ----
void drawBallScene(unsigned long t) {
  drawEyeScene(t);
  int bx, by;
  ballPos(t, bx, by);
  u8g2.setDrawColor(1);
  u8g2.drawDisc(bx, by, 3);
  u8g2.setDrawColor(0);
  u8g2.drawPixel(bx - 1, by - 1);
  u8g2.setDrawColor(1);
}

// ---- heart rain ----
void drawHeartRain(unsigned long t) {
  u8g2.setDrawColor(1);
  for (int i = 0; i < 8; i++) {
    int x = 8 + i * 16 + (int)(3 * sinf(t / 300.0f + i));
    int y = (int)((t / (9 + (i % 3) * 3) + i * 27) % 80) - 8;
    drawHeart(x, y, 2 + (i % 2));
  }
  drawHeart(64, 30, (int)(8 + 2 * sinf(t / 120.0f)));
}

// ---- bouncing letters ----
void drawMeow(unsigned long t) {
  const char *word = (t < 1500) ? "MEOW!" : "PURR~";
  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.setDrawColor(1);
  int len = strlen(word);
  int total = 0;
  char c[2] = {0, 0};
  for (int i = 0; i < len; i++) { c[0] = word[i]; total += u8g2.getStrWidth(c); }
  int x = (128 - total) / 2;
  for (int i = 0; i < len; i++) {
    c[0] = word[i];
    int y = 40 + (int)(7 * sinf(t / 140.0f + i * 0.8f));
    u8g2.drawStr(x, y, c);
    x += u8g2.getStrWidth(c);
  }
  // little paw prints
  for (int i = 0; i < 4; i++) {
    int px = 20 + i * 28, py = 58 - (i % 2) * 3;
    if (((t / 250) % 5) > (unsigned)i) {
      u8g2.drawDisc(px, py, 2);
      u8g2.drawPixel(px - 3, py - 3); u8g2.drawPixel(px, py - 4); u8g2.drawPixel(px + 3, py - 3);
    }
  }
}

// ---- sleeping cat ----
void drawSleepCat(unsigned long t) {
  int br = (int)(2 * sinf(t / 450.0f));   // breathing
  u8g2.setDrawColor(1);

  // tail wrapped underneath
  thickLine(98, 50, 106, 55);
  thickLine(106, 55, 100, 61);
  thickLine(100, 61, 62, 61);

  // body
  u8g2.drawFilledEllipse(66, 46, 34, 13 + br);

  // head with black outline so it separates from the body
  u8g2.setDrawColor(0);
  u8g2.drawCircle(36, 42, 12);
  u8g2.setDrawColor(1);
  u8g2.drawDisc(36, 42, 11);
  u8g2.drawTriangle(26, 38, 28, 25, 37, 32);
  u8g2.drawTriangle(36, 32, 44, 25, 46, 38);

  // closed eyes, nose, stripes
  u8g2.setDrawColor(0);
  u8g2.drawCircle(32, 42, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.drawCircle(41, 42, 3, U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT);
  u8g2.drawPixel(37, 47);
  for (int i = 0; i < 3; i++) u8g2.drawLine(58 + i * 12, 34, 60 + i * 12, 41);
  u8g2.setDrawColor(1);

  // floating Zzz (small -> big)
  const uint8_t *fonts[3] = {u8g2_font_5x7_tr, u8g2_font_6x10_tr, u8g2_font_7x13_tr};
  int shown = min(3, (int)(t / 700) + 1);
  for (int k = 0; k < shown; k++) {
    u8g2.setFont(fonts[k]);
    int bob = (int)(2 * sinf(t / 300.0f + k));
    u8g2.drawStr(78 + k * 11, 24 - k * 8 + bob, "z");
  }
}

// ---- message sent from the phone ----
void drawMessage(unsigned long t) {
  u8g2.setFont(u8g2_font_ncenB14_tr);
  u8g2.setDrawColor(1);
  if (!msgScroll) {
    // short text: bouncing letters, centered
    int len = msgText.length();
    int total = 0;
    char c[2] = {0, 0};
    for (int i = 0; i < len; i++) { c[0] = msgText[i]; total += u8g2.getStrWidth(c); }
    int x = (128 - total) / 2;
    for (int i = 0; i < len; i++) {
      c[0] = msgText[i];
      int y = 36 + (int)(5 * sinf(t / 160.0f + i * 0.7f));
      u8g2.drawStr(x, y, c);
      x += u8g2.getStrWidth(c);
    }
  } else {
    // long text: scrolls right to left once
    int x = 128 - (int)(t * 60UL / 1000UL);
    u8g2.drawStr(x, 38, msgText.c_str());
  }
  // remaining-time bar
  if (msgDur > t) u8g2.drawHLine(0, 62, (int)(128UL * (msgDur - t) / msgDur));
}

// =====================================================
//                  MESSAGE HANDLING
// =====================================================
String sanitize(const String &in) {
  String out;
  for (unsigned i = 0; i < in.length() && out.length() < 120; i++) {
    char ch = in[i];
    if (ch >= 32 && ch <= 126) out += ch;
    else if (ch == '\n' || ch == '\t') out += ' ';
    // bytes >= 128 (emoji, non-English letters) are skipped: the font is ASCII only
  }
  out.trim();
  if (out.length() == 0) out = "?";
  return out;
}

void prepareMessage() {
  u8g2.setFont(u8g2_font_ncenB14_tr);
  int w = u8g2.getStrWidth(msgText.c_str());
  if (w <= 120) {
    msgScroll = false;
    msgDur = 6000;
  } else {
    msgScroll = true;
    msgDur = (unsigned long)(w + 128) * 1000UL / 60UL + 400UL;
    if (msgDur > 60000UL) msgDur = 60000UL;
  }
}

// =====================================================
//                  SCENE CONTROL
// =====================================================
int sceneMood(Scene s) {
  switch (s) {
    case S_NORMAL:    return M_NORMAL;
    case S_CUTE:      return M_CUTE;
    case S_ANGRY:     return M_ANGRY;
    case S_SWEET:     return M_SWEET;
    case S_CAT:       return M_CAT;
    case S_LOVE:      return M_LOVE;
    case S_SURPRISED: return M_SURPRISED;
    case S_SLEEPY:    return M_SLEEPY;
    case S_WINK:      return M_CUTE;
    case S_SAD:       return M_SAD;
    case S_SIDEEYE:   return M_SUSPICIOUS;
    case S_ROLL:      return M_NORMAL;
    case S_BALL:      return M_NORMAL;
    default:          return -1;       // custom-drawn scene
  }
}

void startScene(Scene s) {
  scene = s;
  sceneStart = millis();
  sceneDur = (s == S_MESSAGE) ? msgDur : SCENE_MS;
  int m = sceneMood(s);
  if (m >= 0) mood = (Mood)m;
  nextLook = 0;
}

void refillBag(int avoidFirst) {
  for (int i = 0; i < ANIM_COUNT; i++) bag[i] = i;
  for (int i = ANIM_COUNT - 1; i > 0; i--) {
    int j = random(0, i + 1);
    uint8_t tmp = bag[i]; bag[i] = bag[j]; bag[j] = tmp;
  }
  if (bag[0] == avoidFirst) { uint8_t tmp = bag[0]; bag[0] = bag[1]; bag[1] = tmp; }
  bagPos = 0;
}

void nextScene() {
  if (msgPending) {                     // a queued message goes first
    msgPending = false;
    startScene(S_MESSAGE);
    return;
  }
  if (bagPos >= ANIM_COUNT) refillBag((int)scene);
  startScene((Scene)bag[bagPos++]);
}

void wakeUp() {
  lastActivity = millis();
  sleeping = false;
}

// immediate = show right now; otherwise show after the current animation
void showMessage(const String &s, bool immediate) {
  msgText = sanitize(s);
  prepareMessage();

  if (immediate) {
    wakeUp();
    messageActive = true;
    messageStart = millis();
    scene = S_MESSAGE;
    sceneStart = messageStart;
    sceneDur = msgDur;

    // Draw immediately so the text cannot be overwritten by an animation frame.
    u8g2.clearBuffer();
    u8g2.setDrawColor(1);
    drawMessage(0);
    u8g2.sendBuffer();

    Serial.print("MESSAGE DISPLAY: ");
    Serial.println(msgText);
  } else {
    msgPending = true;
  }
}

void drawScene(unsigned long t) {
  u8g2.clearBuffer();
  u8g2.setDrawColor(1);
  switch (scene) {
    case S_DANCE:     drawDanceCat(t);    break;
    case S_DIZZY:     drawDizzy(t);       break;
    case S_STARS:     drawStarsScene(t);  break;
    case S_CATFACE:   drawCatFace(t);     break;
    case S_WALK:      drawWalkCat(t);     break;
    case S_BALL:      drawBallScene(t);   break;
    case S_HEARTRAIN: drawHeartRain(t);   break;
    case S_MEOW:      drawMeow(t);        break;
    case S_SLEEPCAT:  drawSleepCat(t);    break;
    case S_MESSAGE:   drawMessage(t);     break;
    default:          drawEyeScene(t);    break;
  }
  if (hold) { u8g2.setDrawColor(1); u8g2.drawDisc(123, 3, 2); }   // dot = animation held
  u8g2.sendBuffer();
}

void updateAnimation(unsigned long t) {
  unsigned long now = millis();

  // morph the face toward the current mood (eye scenes only)
  if (sceneMood(scene) >= 0) {
    float *c = (float *)&cur;
    const float *tg = (const float *)&PRESET[mood];
    for (unsigned i = 0; i < sizeof(Face) / sizeof(float); i++) c[i] += (tg[i] - c[i]) * 0.3f;
  }

  // random look-around
  if (now > nextLook) {
    tLookX = random(-100, 101) / 100.0f;
    tLookY = random(-60, 61) / 100.0f;
    nextLook = now + random(900, 2500);
  }
  if (scene == S_SLEEPY)  { tLookX = 0; tLookY = 0.5f; }
  if (scene == S_SIDEEYE) { tLookX = ((t / 800) % 2 == 0) ? 1.0f : -1.0f; tLookY = 0.1f; }
  lookX += (tLookX - lookX) * 0.25f;
  lookY += (tLookY - lookY) * 0.25f;

  // scripted looks
  if (scene == S_ROLL) {
    float ang = t / 260.0f;
    lookX = cosf(ang);
    lookY = 0.8f * sinf(ang);
  }
  if (scene == S_BALL) {
    int bx, by;
    ballPos(t, bx, by);
    lookX = constrain((bx - 64) / 52.0f, -1.0f, 1.0f);
    lookY = 0.7f;
  }

  // random blink
  if (now > nextBlink && !blinking) {
    blinking = true;
    blinkStart = now;
    nextBlink = now + random(2000, 5000);
  }
}

// =====================================================
//                 WEB PAGE + HANDLERS
// =====================================================
const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cat Keychain</title>
<style>
body{font-family:-apple-system,Arial,sans-serif;background:#111;color:#fff;margin:0 auto;padding:16px;max-width:480px}
h1{font-size:22px;text-align:center}
input{width:100%;box-sizing:border-box;padding:14px;font-size:17px;border-radius:12px;border:0;margin-bottom:10px}
button{padding:12px;font-size:15px;border-radius:12px;border:0;background:#ff8fab;color:#111;font-weight:600}
.big{width:100%;font-size:18px;padding:14px}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:12px}
.alt{background:#2a2a2a;color:#fff}
#s{text-align:center;height:20px;color:#9f9;margin:8px 0}
</style></head><body>
<h1>Cat Keychain</h1>
<input id="t" maxlength="100" placeholder="Type a message..." autocomplete="off">
<button class="big" onclick="send()">Send to display</button>
<div id="s"></div>
<div class="grid">
<button class="alt" onclick="call('/next')">Next</button>
<button class="alt" onclick="call('/hold?on=1')">Hold</button>
<button class="alt" onclick="call('/hold?on=0')">Auto</button>
</div>
<h3>Animations</h3>
<div class="grid" id="g"></div>
<script>
var N=["Dance","Normal","Cute","Angry","Sweet","Cat","Love","Surprised","Sleepy","Wink","Dizzy","Stars","Sad","Side-eye","Eye roll","Cat face","Walk","Ball","Hearts","Meow","Sleep cat"];
var g=document.getElementById('g'),s=document.getElementById('s');
N.forEach(function(n,i){var b=document.createElement('button');b.className='alt';b.textContent=n;b.onclick=function(){call('/scene?id='+i)};g.appendChild(b)});
function call(u){fetch(u).then(function(){s.textContent='OK'}).catch(function(){s.textContent='No connection'})}
function send(){var t=document.getElementById('t');var v=t.value.trim();if(!v)return;fetch('/send?text='+encodeURIComponent(v),{cache:'no-store'}).then(function(r){return r.text()}).then(function(x){s.textContent=x}).catch(function(){s.textContent='No connection'});t.value=''}
document.getElementById('t').addEventListener('keydown',function(e){if(e.key==='Enter')send()});
</script></body></html>
)rawliteral";

void handleRoot()  { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }

void handleSend() {
  String text = server.arg("text");
  Serial.println("HTTP /send received");
  Serial.print("RAW TEXT: [");
  Serial.print(text);
  Serial.println("]");

  if (text.length() == 0) {
    server.send(400, "text/plain", "EMPTY TEXT");
    return;
  }

  showMessage(text, true);
  server.send(200, "text/plain", "MESSAGE SHOWN");
}

void handleScene() {
  int id = server.arg("id").toInt();
  if (id >= 0 && id < ANIM_COUNT) { wakeUp(); startScene((Scene)id); }
  server.send(200, "text/plain", "OK");
}

void handleNext() {
  wakeUp();
  nextScene();
  server.send(200, "text/plain", "OK");
}

void handleHold() {
  hold = (server.arg("on") == "1");
  wakeUp();
  server.send(200, "text/plain", "OK");
}

void startServer() {
  if (serverStarted) return;
  server.on("/", handleRoot);
  server.on("/send", handleSend);
  server.on("/scene", handleScene);
  server.on("/next", handleNext);
  server.on("/hold", handleHold);
  server.begin();
  serverStarted = true;
}

void wifiTask() {
  unsigned long now = millis();

  // connected to the iPhone hotspot
  if (WiFi.status() == WL_CONNECTED && !staAnnounced) {
    staAnnounced = true;
    startServer();
    if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);
    showMessage("Online! http://" + WiFi.localIP().toString() + " or " + HOSTNAME + ".local", false);
  }

  // could not join in 15 s -> make our own WiFi so the phone can still connect
  if (!apStarted && !staAnnounced && (now - wifiBeganAt) > 15000UL) {
    apStarted = true;
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS);
    WiFi.begin(WIFI_SSID, WIFI_PASS);    // keep trying the hotspot in the background
    startServer();
    showMessage(String("No WiFi. Join ") + AP_SSID + " then open 192.168.4.1", false);
  }
}

// =====================================================
//                BUTTON / POWER
// =====================================================
void goToSleep() {
  u8g2.setPowerSave(1);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << BTN_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_deep_sleep_start();
}

void handleButton() {
  unsigned long now = millis();
  bool down = (digitalRead(BTN_PIN) == LOW);

  if (down && !btnDown) { btnDown = true; pressStart = now; }
  if (!down && btnDown) {
    btnDown = false;
    unsigned long held = now - pressStart;
    if (held > 30) {
      wakeUp();
      if (held > 800) hold = !hold;     // long press: hold / release current animation
      else nextScene();                 // short press: next animation
    }
  }
}

// =====================================================
//                    SETUP / LOOP
// =====================================================
void setup() {
  pinMode(BTN_PIN, INPUT_PULLUP);

  // if we just woke from deep sleep the button may still be pressed: wait for release
  unsigned long w = millis();
  while (digitalRead(BTN_PIN) == LOW && millis() - w < 2000) delay(10);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);

  u8g2.setI2CAddress(0x3D * 2);         // U8g2 wants the 8-bit address
  u8g2.begin();
  u8g2.setContrast(200);

  // WiFi connects in the background while the cat dances
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // fixes weak-antenna connect problems on many C3 Super Minis
  wifiBeganAt = millis();

  randomSeed(esp_random() ^ micros());
  refillBag(S_DANCE);                    // bag never starts with the dance
  startScene(S_DANCE);                   // 3 seconds of dancing cat first
  nextBlink = millis() + 2000;
  lastActivity = millis();
}

void loop() {
  unsigned long now = millis();
  handleButton();
  wifiTask();
  if (serverStarted) server.handleClient();

  // PHONE MESSAGE HAS PRIORITY OVER ALL ANIMATIONS
  if (messageActive) {
    unsigned long mt = now - messageStart;

    if (mt >= msgDur) {
      messageActive = false;
      Serial.println("MESSAGE DISPLAY: OFF");
      nextScene();
    } else {
      u8g2.clearBuffer();
      u8g2.setDrawColor(1);
      drawMessage(mt);
      u8g2.sendBuffer();
      delay(5);
      return;
    }
  }

  // idle -> show sleeping cat for 3 s, then deep sleep
  if (IDLE_SLEEP_MS > 0 && !sleeping && (now - lastActivity) > IDLE_SLEEP_MS) {
    sleeping = true;
    startScene(S_SLEEPCAT);
  }
  if (sleeping) {
    if (now - sceneStart >= sceneDur) goToSleep();
  } else if (!hold && now - sceneStart >= sceneDur) {
    nextScene();                         // animations advance every 3 seconds
  }

  unsigned long t = (now - sceneStart) % sceneDur;
  updateAnimation(t);
  drawScene(t);
  delay(5);
}