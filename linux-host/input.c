/* PSP buttons/analog -> Linux uinput virtual Xbox 360 gamepad, with a
   user-editable mapping (see input.conf.example). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include "pspdisp.h"

static int fd = -1;
volatile int g_input_enabled = 1;   /* toggled at runtime via SIGUSR1 */

/* virtual pad outputs */
enum { O_NONE, O_A, O_B, O_X, O_Y, O_LB, O_RB, O_LT, O_RT, O_BACK, O_START, O_GUIDE,
       O_L3, O_R3, O_UP, O_DOWN, O_LEFT, O_RIGHT, O_FACE_STICK, O_COUNT };

static const struct { const char *name; int out; int key; } outs[] = {
  { "none", O_NONE, 0 },           { "a", O_A, BTN_SOUTH },      { "b", O_B, BTN_EAST },
  { "x", O_X, BTN_NORTH },          { "y", O_Y, BTN_WEST },      { "lb", O_LB, BTN_TL },
  { "rb", O_RB, BTN_TR },          { "lt", O_LT, 0 },            { "rt", O_RT, 0 },
  { "back", O_BACK, BTN_SELECT },  { "start", O_START, BTN_START }, { "guide", O_GUIDE, BTN_MODE },
  { "l3", O_L3, BTN_THUMBL },      { "r3", O_R3, BTN_THUMBR },
  { "up", O_UP, 0 }, { "down", O_DOWN, 0 }, { "left", O_LEFT, 0 }, { "right", O_RIGHT, 0 },
  { "face_stick", O_FACE_STICK, 0 },
};
#define NOUTS (int)(sizeof(outs)/sizeof(outs[0]))

static const struct { const char *name; uint32_t bit; } ins[] = {
  { "cross", PSP_CROSS }, { "circle", PSP_CIRCLE }, { "square", PSP_SQUARE },
  { "triangle", PSP_TRIANGLE }, { "l", PSP_LTRIG }, { "r", PSP_RTRIG },
  { "start", PSP_START }, { "select", PSP_SELECT },
  { "ps", PSP_HOME },
  { "up", PSP_UP }, { "down", PSP_DOWN }, { "left", PSP_LEFT }, { "right", PSP_RIGHT },
};
#define NINS (int)(sizeof(ins)/sizeof(ins[0]))

#define MAXRULES 64
static struct { uint32_t mods, btn; int out; } rules[MAXRULES];
static int nrules;
enum { STICK_LEFT, STICK_RIGHT, STICK_NONE };
static int stick_mode = STICK_LEFT;

static void add_rule(uint32_t mods, uint32_t btn, int out)
{
  for (int i = 0; i < nrules; i++)
    if (rules[i].mods == mods && rules[i].btn == btn) { rules[i].out = out; return; }
  if (nrules < MAXRULES) { rules[nrules].mods = mods; rules[nrules].btn = btn; rules[nrules].out = out; nrules++; }
}

static int find_in(const char *s)  { for (int i = 0; i < NINS; i++)  if (!strcmp(s, ins[i].name))  return i; return -1; }
static int find_out(const char *s) { for (int i = 0; i < NOUTS; i++) if (!strcmp(s, outs[i].name)) return outs[i].out; return -1; }

static void load_defaults(void)
{
  nrules = 0; stick_mode = STICK_LEFT;
  static const char *d[][2] = {
    {"cross","a"},{"circle","b"},{"square","x"},{"triangle","y"},{"l","lb"},{"r","rb"},
    {"start","start"},{"select","back"},{"up","up"},{"down","down"},{"left","left"},{"right","right"},
    {"ps+l","lt"},{"ps+r","rt"},{"ps+select","face_stick"},
  };
  for (unsigned i = 0; i < sizeof d / sizeof d[0]; i++) {
    char k[32]; strcpy(k, d[i][0]);
    char *plus = strchr(k, '+'); uint32_t mods = 0; char *b = k;
    if (plus) { *plus = 0; mods = ins[find_in(k)].bit; b = plus + 1; }
    add_rule(mods, ins[find_in(b)].bit, find_out(d[i][1]));
  }
}

static char *trim(char *s)
{
  while (isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
  return s;
}

static void load_config(const char *path)
{
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[128]; int n = 0;
  while (fgets(line, sizeof line, f)) {
    n++;
    char *h = strchr(line, '#'); if (h) *h = 0;
    char *eq = strchr(line, '='); if (!eq) continue;
    *eq = 0;
    char *key = trim(line), *val = trim(eq + 1);
    for (char *p = key; *p; p++) *p = tolower((unsigned char)*p);
    for (char *p = val; *p; p++) *p = tolower((unsigned char)*p);
    if (!strcmp(key, "stick")) {
      stick_mode = !strcmp(val, "right") ? STICK_RIGHT : !strcmp(val, "none") ? STICK_NONE : STICK_LEFT;
      continue;
    }
    uint32_t mods = 0; char *b = key;
    for (char *plus; (plus = strchr(b, '+')); b = plus + 1) {
      *plus = 0; int m = find_in(trim(b));
      if (m < 0) { fprintf(stderr, "%s:%d: unknown button '%s'\n", path, n, b); mods = ~0u; break; }
      mods |= ins[m].bit;
    }
    int bi = find_in(trim(b)), o = find_out(val);
    if (mods == ~0u) continue;
    if (bi < 0 || o < 0) { fprintf(stderr, "%s:%d: bad mapping '%s = %s'\n", path, n, key, val); continue; }
    add_rule(mods, ins[bi].bit, o);
  }
  fclose(f);
  fprintf(stderr, "input: loaded mapping from %s\n", path);
}

static void absinfo(int code, int min, int max, int flat)
{
  struct uinput_abs_setup a; memset(&a, 0, sizeof a);
  a.code = code; a.absinfo.minimum = min; a.absinfo.maximum = max; a.absinfo.flat = flat;
  ioctl(fd, UI_SET_ABSBIT, code);
  ioctl(fd, UI_ABS_SETUP, &a);
}

bool input_init(const char *conf_path)
{
  load_defaults();
  char buf[512];
  if (!conf_path) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(buf, sizeof buf, "%s/pspdisp/input.conf", xdg);
    else if (home)   snprintf(buf, sizeof buf, "%s/.config/pspdisp/input.conf", home);
    else buf[0] = 0;
    conf_path = buf;
  }
  if (*conf_path) load_config(conf_path);

  fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) { fprintf(stderr, "input: open /dev/uinput failed (udev rule / root)\n"); return false; }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  for (int i = 0; i < NOUTS; i++) if (outs[i].key) ioctl(fd, UI_SET_KEYBIT, outs[i].key);

  struct uinput_setup us; memset(&us, 0, sizeof us);
  us.id.bustype = BUS_USB; us.id.vendor = 0x045E; us.id.product = 0x028E; us.id.version = 0x0110;
  strcpy(us.name, "Microsoft X-Box 360 pad");
  ioctl(fd, UI_DEV_SETUP, &us);

  absinfo(ABS_X,  -32768, 32767, 4096);
  absinfo(ABS_Y,  -32768, 32767, 4096);
  absinfo(ABS_RX, -32768, 32767, 4096);
  absinfo(ABS_RY, -32768, 32767, 4096);
  absinfo(ABS_Z,  0, 255, 0);
  absinfo(ABS_RZ, 0, 255, 0);
  absinfo(ABS_HAT0X, -1, 1, 0);
  absinfo(ABS_HAT0Y, -1, 1, 0);

  ioctl(fd, UI_DEV_CREATE);
  return true;
}

static void emit(int type, int code, int val)
{
  struct input_event ev; memset(&ev, 0, sizeof ev);
  ev.type = type; ev.code = code; ev.value = val;
  if (write(fd, &ev, sizeof ev) < 0) { /* ignore */ }
}

static int axis16(uint8_t v) { return ((int)v - 128) * 256; }

/* best (most chord modifiers) rule for a physical button given what is held */
static int best_rule(uint32_t held, uint32_t btn)
{
  int best = -1, bestn = -1;
  for (int i = 0; i < nrules; i++) {
    if (rules[i].btn != btn || (rules[i].mods & held) != rules[i].mods) continue;
    int n = __builtin_popcount(rules[i].mods);
    if (n > bestn) { best = i; bestn = n; }
  }
  return best;
}

#define FACE_MASK (PSP_CROSS | PSP_CIRCLE | PSP_SQUARE | PSP_TRIANGLE)

void input_update(uint32_t buttons, uint8_t ax, uint8_t ay)
{
  static bool last[O_COUNT];
  static bool face_stick;   /* face buttons act as the right stick */
  if (fd < 0) return;
  if (!g_input_enabled) { buttons = 0; ax = ay = 128; }

  bool now[O_COUNT] = { false };
  uint32_t consumed = 0;
  for (int i = 0; i < NINS; i++) {
    if (!(buttons & ins[i].bit)) continue;
    int r = best_rule(buttons, ins[i].bit);
    if (r >= 0) consumed |= rules[r].mods;
  }
  for (int i = 0; i < NINS; i++) {
    if (!(buttons & ins[i].bit) || (consumed & ins[i].bit)) continue;
    if (face_stick && (ins[i].bit & FACE_MASK)) continue;
    int r = best_rule(buttons, ins[i].bit);
    if (r >= 0) now[rules[r].out] = true;
  }

  if (now[O_FACE_STICK] && !last[O_FACE_STICK]) face_stick = !face_stick;

  for (int i = 0; i < NOUTS; i++) {
    int o = outs[i].out;
    if (outs[i].key && now[o] != last[o]) emit(EV_KEY, outs[i].key, now[o]);
    last[o] = now[o];
  }

  int lx = 0, ly = 0, rx = 0, ry = 0;
  if (stick_mode == STICK_LEFT)  { lx = axis16(ax); ly = axis16(ay); }
  if (stick_mode == STICK_RIGHT) { rx = axis16(ax); ry = axis16(ay); }
  if (face_stick && !(consumed & FACE_MASK)) {
    rx = ((buttons & PSP_CIRCLE) ? 32767 : 0) - ((buttons & PSP_SQUARE) ? 32767 : 0);
    ry = ((buttons & PSP_CROSS) ? 32767 : 0) - ((buttons & PSP_TRIANGLE) ? 32767 : 0);
  }
  emit(EV_ABS, ABS_X, lx);  emit(EV_ABS, ABS_Y, ly);
  emit(EV_ABS, ABS_RX, rx); emit(EV_ABS, ABS_RY, ry);
  emit(EV_ABS, ABS_Z,  now[O_LT] ? 255 : 0);
  emit(EV_ABS, ABS_RZ, now[O_RT] ? 255 : 0);
  emit(EV_ABS, ABS_HAT0X, (now[O_RIGHT] ? 1 : 0) - (now[O_LEFT] ? 1 : 0));
  emit(EV_ABS, ABS_HAT0Y, (now[O_DOWN] ? 1 : 0) - (now[O_UP] ? 1 : 0));
  emit(EV_SYN, SYN_REPORT, 0);
}

void input_term(void)
{
  if (fd >= 0) { ioctl(fd, UI_DEV_DESTROY); close(fd); fd = -1; }
}
