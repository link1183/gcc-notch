#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "engine.h"
#include <math.h>
#include <raygui.h>
#include <stdio.h>
#include <string.h>

#define W 1060
#define H 656

/* ---- palette (dark) ---------------------------------------------------- */
static const Color BG = {15, 17, 23, 255};       /* window background      */
static const Color PANEL = {25, 28, 38, 255};    /* card background       */
static const Color PANEL2 = {19, 21, 29, 255};   /* inset / plot bg       */
static const Color LINE = {42, 47, 61, 255};     /* borders / separators  */
static const Color GRID = {38, 43, 56, 255};     /* faint grid            */
static const Color TXT = {206, 212, 224, 255};   /* primary text          */
static const Color DIM = {120, 130, 150, 255};   /* secondary text        */
static const Color ACCENT = {91, 157, 255, 255}; /* blue accent           */
static const Color GOOD = {70, 214, 160, 255};   /* green                 */
static const Color BAD = {255, 93, 108, 255};    /* red                   */
static const Color WARN = {255, 196, 87, 255};   /* amber                 */

static Font FONT, FONTB; /* regular + semibold */

/* ---- text helpers ------------------------------------------------------ */
static void txt(Font f, const char *s, float x, float y, float sz, Color c) {
  DrawTextEx(f, s, (Vector2){x, y}, sz, sz / 16.0f, c);
}

/* ---- shape helpers ----------------------------------------------------- */
static void card(Rectangle r) {
  DrawRectangleRounded(r, 0.045f, 8, PANEL);
  DrawRectangleRoundedLinesEx(r, 0.045f, 8, 1.0f, LINE);
}
/* soft glowing dot: a halo behind a solid core */
static void glow_dot(Vector2 p, float r, Color c) {
  DrawCircleV(p, r * 3.2f, Fade(c, 0.10f));
  DrawCircleV(p, r * 2.0f, Fade(c, 0.18f));
  DrawCircleV(p, r, c);
  DrawCircleV(p, r * 0.45f, Fade(WHITE, 0.65f));
}
/* fill a fan triangle regardless of winding order */
static void tri2(Vector2 a, Vector2 b, Vector2 cc, Color col) {
  DrawTriangle(a, b, cc, col);
  DrawTriangle(a, cc, b, col);
}

static Vector2 rts(Rectangle a, double rx, double ry) {
  int lo = eng_axis_min(), hi = eng_axis_max();
  double t = hi - lo;
  if (t <= 0)
    t = 255;
  return (Vector2){a.x + (float)((rx - lo) / t) * a.width,
                   a.y + (float)((ry - lo) / t) * a.height};
}

static const char *shortname(const char *s) {
  if (!strncmp(s, "BTN_", 4) || !strncmp(s, "KEY_", 4) ||
      !strncmp(s, "ABS_", 4))
    return s + 4;
  return s;
}

static void draw_stick(Rectangle outer, const char *title, int stick) {
  card(outer);
  txt(FONTB, title, outer.x + 16, outer.y + 12, 17, TXT);

  /* plot area inset below the title */
  Rectangle a = {outer.x + 16, outer.y + 40, outer.width - 32,
                 outer.height - 56};
  DrawRectangleRounded(a, 0.03f, 6, PANEL2);

  Vector2 ctr = rts(a, 128, 128);

  /* range circle */
  DrawCircleLinesV(ctr, a.width * 0.46f, GRID);

  /* grid: quarter divisions */
  for (int i = 1; i < 4; i++) {
    float fx = a.x + a.width * i / 4.0f;
    float fy = a.y + a.height * i / 4.0f;
    DrawLineV((Vector2){fx, a.y}, (Vector2){fx, a.y + a.height}, GRID);
    DrawLineV((Vector2){a.x, fy}, (Vector2){a.x + a.width, fy}, GRID);
  }
  /* crosshair through center, a touch brighter */
  DrawLineV((Vector2){a.x, ctr.y}, (Vector2){a.x + a.width, ctr.y},
            Fade(DIM, 0.35f));
  DrawLineV((Vector2){ctr.x, a.y}, (Vector2){ctr.x, a.y + a.height},
            Fade(DIM, 0.35f));

  const eng_stick_cal *c = eng_cal(stick);
  if (eng_has_cal()) {
    /* ideal octagon: soft fill + crisp outline */
    Vector2 ideal[ENG_NOTCH];
    for (int i = 0; i < ENG_NOTCH; i++) {
      double wx, wy;
      eng_ideal_vec(stick, i, &wx, &wy);
      ideal[i] = rts(a, 128 + wx, 128 + wy);
    }
    for (int i = 0; i < ENG_NOTCH; i++)
      tri2(ctr, ideal[i], ideal[(i + 1) % ENG_NOTCH], Fade(GOOD, 0.06f));
    for (int i = 0; i < ENG_NOTCH; i++)
      DrawLineEx(ideal[i], ideal[(i + 1) % ENG_NOTCH], 1.5f, Fade(GOOD, 0.55f));

    /* measured notch polygon: blue outline + nodes */
    Vector2 meas[ENG_NOTCH];
    for (int i = 0; i < ENG_NOTCH; i++) {
      double vx, vy;
      eng_measured_vec(stick, i, &vx, &vy);
      meas[i] = rts(a, c->cx + vx, c->cy + vy);
    }
    for (int i = 0; i < ENG_NOTCH; i++)
      DrawLineEx(meas[i], meas[(i + 1) % ENG_NOTCH], 2.0f, Fade(ACCENT, 0.75f));
    for (int i = 0; i < ENG_NOTCH; i++) {
      DrawCircleV(meas[i], 4.5f, Fade(ACCENT, 0.22f));
      DrawCircleV(meas[i], 2.5f, ACCENT);
    }
  }

  int rx = eng_raw(c->ax), ry = eng_raw(c->ay);
  if (eng_has_cal()) {
    int ox, oy;
    eng_remap_point(stick, rx, ry, &ox, &oy);
    /* line from raw -> remapped to show the correction */
    DrawLineEx(rts(a, rx, ry), rts(a, ox, oy), 1.5f, Fade(GOOD, 0.35f));
    glow_dot(rts(a, ox, oy), 5.0f, GOOD);
  }
  glow_dot(rts(a, rx, ry), 5.0f, BAD);
}

/* a small legend chip: colored dot + label */
static float chip(float x, float y, Color c, const char *label) {
  DrawCircleV((Vector2){x + 5, y + 7}, 5, c);
  txt(FONT, label, x + 16, y, 13, DIM);
  return x + 16 + MeasureTextEx(FONT, label, 13, 13 / 16.0f).x + 22;
}

int main(int argc, char **argv) {
  SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
  InitWindow(W, H, "GCC Notch Remapper");
  SetTargetFPS(240);

  /* crisp TTF; fall back to the built-in font if unavailable */
  const char *reg = "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-Regular.ttf";
  const char *bld = "/usr/share/fonts/TTF/JetBrainsMonoNerdFont-SemiBold.ttf";
  FONT = FileExists(reg) ? LoadFontEx(reg, 48, NULL, 0) : GetFontDefault();
  FONTB = FileExists(bld) ? LoadFontEx(bld, 48, NULL, 0) : FONT;
  SetTextureFilter(FONT.texture, TEXTURE_FILTER_BILINEAR);
  SetTextureFilter(FONTB.texture, TEXTURE_FILTER_BILINEAR);

  /* raygui dark theme */
  GuiSetFont(FONT);
  GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
  GuiSetStyle(DEFAULT, BORDER_WIDTH, 1);
  GuiSetStyle(DEFAULT, BACKGROUND_COLOR, ColorToInt(BG));
  GuiSetStyle(DEFAULT, LINE_COLOR, ColorToInt(LINE));
  GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL, ColorToInt(LINE));
  GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt((Color){35, 39, 52, 255}));
  GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL, ColorToInt(TXT));
  GuiSetStyle(DEFAULT, BORDER_COLOR_FOCUSED, ColorToInt(ACCENT));
  GuiSetStyle(DEFAULT, BASE_COLOR_FOCUSED,
              ColorToInt((Color){44, 50, 66, 255}));
  GuiSetStyle(DEFAULT, TEXT_COLOR_FOCUSED, ColorToInt(WHITE));
  GuiSetStyle(DEFAULT, BORDER_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(DEFAULT, BASE_COLOR_PRESSED, ColorToInt(ACCENT));
  GuiSetStyle(DEFAULT, TEXT_COLOR_PRESSED, ColorToInt(BG));

  eng_load_cfg();
  eng_open(argc > 1 ? argv[1] : NULL);

  while (!WindowShouldClose()) {
    eng_poll();

    BeginDrawing();
    ClearBackground(BG);

    /* ---- header -------------------------------------------------------- */
    DrawRectangle(0, 0, W, 64, PANEL);
    DrawLine(0, 64, W, 64, LINE);
    txt(FONTB, "GCC Notch Remapper", 28, 14, 24, TXT);
    txt(FONT, "GameCube controller calibration & notch remapping", 28, 42, 13,
        DIM);

    /* status pill, top-right */
    bool conn = eng_connected();
    Color sc = conn ? (eng_has_cal() ? GOOD : WARN) : BAD;
    const char *st =
        conn ? (eng_has_cal() ? "ready" : "not calibrated") : "disconnected";
    float pw = MeasureTextEx(FONT, st, 14, 14 / 16.0f).x + 44;
    Rectangle pill = {W - 28 - pw, 18, pw, 28};
    DrawRectangleRounded(pill, 1.0f, 12, PANEL2);
    DrawRectangleRoundedLinesEx(pill, 1.0f, 12, 1.0f, Fade(sc, 0.5f));
    DrawCircleV((Vector2){pill.x + 18, pill.y + 14}, 5, sc);
    txt(FONT, st, pill.x + 30, pill.y + 6, 14, TXT);

    /* ---- stick plots --------------------------------------------------- */
    draw_stick((Rectangle){28, 84, 312, 312}, "Control Stick", 0);
    draw_stick((Rectangle){352, 84, 312, 312}, "C-Stick", 1);

    /* legend */
    float lx = 28, ly = 406;
    lx = chip(lx, ly, BAD, "raw");
    lx = chip(lx, ly, GOOD, "remapped");
    lx = chip(lx, ly, ACCENT, "measured notches");
    lx = chip(lx, ly, Fade(GOOD, 0.7f), "ideal octagon");
    (void)lx;

    /* ---- right column: buttons & axes ---------------------------------- */
    Rectangle bpanel = {684, 84, W - 684 - 28, 300};
    card(bpanel);
    txt(FONTB, "Buttons", bpanel.x + 16, bpanel.y + 12, 17, TXT);
    int codes[64];
    int n = eng_list_keys(codes, 64);
    int colw = (int)(bpanel.width - 32) / 2;
    int bx = (int)bpanel.x + 16, by = (int)bpanel.y + 44, col = 0;
    for (int i = 0; i < n; i++) {
      bool on = eng_key(codes[i]);
      Rectangle led = {(float)bx, (float)by, 14, 14};
      DrawRectangleRounded(led, 0.35f, 4, on ? GOOD : PANEL2);
      if (!on)
        DrawRectangleRoundedLinesEx(led, 0.35f, 4, 1.0f, LINE);
      txt(FONT, shortname(eng_code_name_key(codes[i])), bx + 22, by - 1, 13,
          on ? TXT : DIM);
      by += 22;
      if (by > bpanel.y + bpanel.height - 24) {
        col++;
        by = (int)bpanel.y + 44;
        bx = (int)bpanel.x + 16 + col * colw;
      }
    }

    Rectangle apanel = {684, 400, W - 684 - 28, 168};
    card(apanel);
    txt(FONTB, "Axes / Triggers", apanel.x + 16, apanel.y + 12, 17, TXT);
    int ac[16];
    int m = eng_list_extra_abs(ac, 16);
    float ty = apanel.y + 44;
    for (int k = 0; k < m; k++) {
      int code = ac[k], v = eng_raw(code), lo = eng_abs_min(code),
          hi = eng_abs_max(code);
      bool tg = eng_is_trig(code);
      int outv = eng_trig_out(code, v);
      float fr = (hi > lo) ? (float)(v - lo) / (hi - lo) : 0;    /* raw */
      float fo = (hi > lo) ? (float)(outv - lo) / (hi - lo) : 0; /* remapped */
      txt(FONT, shortname(eng_code_name_abs(code)), apanel.x + 16, ty, 13, DIM);
      Rectangle bar = {apanel.x + 96, ty + 1, apanel.width - 96 - 16 - 50, 12};
      DrawRectangleRounded(bar, 1.0f, 6, PANEL2);
      float shown = tg ? fo : fr;
      if (shown > 0.001f) {
        Rectangle fill = {bar.x, bar.y, bar.width * shown, bar.height};
        DrawRectangleRounded(fill, 1.0f, 6, tg ? GOOD : ACCENT);
      }
      if (tg) {
        /* tick marking the actual raw (pre-rescale) position */
        float mx = bar.x + bar.width * fr;
        DrawLineEx((Vector2){mx, bar.y - 3},
                   (Vector2){mx, bar.y + bar.height + 3}, 1.5f,
                   Fade(DIM, 0.9f));
      }
      txt(FONT, TextFormat("%d%%", (int)(shown * 100 + 0.5f)),
          bar.x + bar.width + 8, ty, 13, tg ? GOOD : DIM);
      ty += 24;
    }
    if (!eng_has_trig())
      txt(FONT, "triggers not calibrated", apanel.x + 16,
          apanel.y + apanel.height - 24, 12, DIM);

    /* ---- controls panel ------------------------------------------------ */
    Rectangle cp = {28, 436, 636, 132};
    card(cp);
    txt(FONTB, "Controls", cp.x + 16, cp.y + 12, 17, TXT);

    float row1 = cp.y + 44;
    if (GuiButton((Rectangle){cp.x + 16, row1, 150, 34},
                  eng_remap_active() ? "Stop Remap" : "Start Remap")) {
      if (eng_remap_active())
        eng_stop_remap();
      else
        eng_start_remap();
    }
    if (GuiButton((Rectangle){cp.x + 176, row1, 150, 34}, "Calibrate Sticks"))
      eng_cal_begin();
    if (GuiButton((Rectangle){cp.x + 336, row1, 170, 34}, "Calibrate Triggers"))
      eng_trig_begin();

    float row2 = cp.y + 88;
    float df = (float)eng_get_diag();
    GuiSliderBar((Rectangle){cp.x + 72, row2, 150, 30}, "Diag",
                 TextFormat("%.2f", df), &df, 0.30f, 1.0f);
    if (fabsf(df - (float)eng_get_diag()) > 1e-4f)
      eng_set_diag(df);
    if (GuiButton((Rectangle){cp.x + 266, row2, 104, 30}, "Reload cfg"))
      eng_load_cfg();
    if (GuiButton((Rectangle){cp.x + 382, row2, cp.width - 398, 30},
                  TextFormat("Port:  %s", eng_dev_path())))
      eng_dev_next();

    /* ---- calibration overlay ------------------------------------------- */
    if (eng_cal_active()) {
      DrawRectangle(0, 0, W, H, Fade((Color){5, 6, 9, 255}, 0.72f));
      Rectangle box = {W / 2.0f - 250, H / 2.0f - 120, 500, 240};
      DrawRectangleRounded(box, 0.06f, 10, PANEL);
      DrawRectangleRoundedLinesEx(box, 0.06f, 10, 1.0f, Fade(ACCENT, 0.6f));

      const char *sn = eng_cal_stick() == 0 ? "Control Stick" : "C-Stick";
      int ph = eng_cal_phase();
      txt(FONTB, "Calibration", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT, sn, box.x + 28, box.y + 52, 14, ACCENT);

      const char *msg;
      char buf[160];
      if (ph == 0)
        msg = "Spin the stick fully around its gate, then press Next.";
      else if (ph == 1)
        msg = "Let the stick rest at center, then press Capture.";
      else {
        snprintf(buf, sizeof buf, "Hold notch %d / 8, then press Capture.",
                 eng_cal_notch() + 1);
        msg = buf;
      }
      txt(FONT, msg, box.x + 28, box.y + 92, 16, TXT);

      /* step dots */
      for (int i = 0; i < 3; i++)
        DrawCircleV((Vector2){box.x + 28 + i * 16, box.y + 128}, 4,
                    i <= (ph > 2 ? 2 : ph) ? ACCENT : LINE);

      const eng_stick_cal *c = eng_cal(eng_cal_stick());
      txt(FONT, TextFormat("live  (%d, %d)", eng_raw(c->ax), eng_raw(c->ay)),
          box.x + 28, box.y + 150, 14, DIM);

      if (GuiButton((Rectangle){box.x + 28, box.y + 182, 150, 38},
                    ph == 0 ? "Next" : "Capture"))
        eng_cal_advance();
      if (GuiButton((Rectangle){box.x + 322, box.y + 182, 150, 38}, "Cancel"))
        eng_cal_cancel();
    }

    /* ---- trigger calibration overlay ----------------------------------- */
    if (eng_trig_active()) {
      DrawRectangle(0, 0, W, H, Fade((Color){5, 6, 9, 255}, 0.72f));
      Rectangle box = {W / 2.0f - 250, H / 2.0f - 130, 500, 260};
      DrawRectangleRounded(box, 0.06f, 10, PANEL);
      DrawRectangleRoundedLinesEx(box, 0.06f, 10, 1.0f, Fade(ACCENT, 0.6f));

      int ph = eng_trig_phase();
      txt(FONTB, "Trigger Calibration", box.x + 28, box.y + 22, 22, TXT);
      txt(FONT, "Calibrates every analog L/R trigger", box.x + 28, box.y + 52,
          14, ACCENT);

      const char *msg = ph == 0 ? "Let both triggers rest (fully released)."
                                : "Now squeeze both triggers all the way down.";
      txt(FONT, msg, box.x + 28, box.y + 86, 16, TXT);

      for (int i = 0; i < 2; i++)
        DrawCircleV((Vector2){box.x + 28 + i * 16, box.y + 118}, 4,
                    i <= ph ? ACCENT : LINE);

      /* live readout of each analog axis */
      int ac2[16];
      int m2 = eng_list_extra_abs(ac2, 16);
      float ly = box.y + 138;
      for (int k = 0; k < m2; k++) {
        int code = ac2[k], lo = eng_abs_min(code), hi = eng_abs_max(code);
        if (hi - lo < 16)
          continue;
        float f = (float)(eng_raw(code) - lo) / (hi - lo);
        txt(FONT, shortname(eng_code_name_abs(code)), box.x + 28, ly, 13, DIM);
        Rectangle bar = {box.x + 150, ly + 1, 220, 10};
        DrawRectangleRounded(bar, 1.0f, 6, PANEL2);
        if (f > 0.001f)
          DrawRectangleRounded(
              (Rectangle){bar.x, bar.y, bar.width * f, bar.height}, 1.0f, 6,
              ACCENT);
        ly += 20;
        if (ly > box.y + 188)
          break;
      }

      if (GuiButton((Rectangle){box.x + 28, box.y + 202, 150, 38},
                    ph == 0 ? "Next" : "Capture"))
        eng_trig_advance();
      if (GuiButton((Rectangle){box.x + 322, box.y + 202, 150, 38}, "Cancel"))
        eng_trig_cancel();
    }

    EndDrawing();
  }

  eng_close();
  CloseWindow();
  return 0;
}
