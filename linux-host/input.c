/* PSP buttons/analog -> Linux uinput virtual Xbox 360 gamepad. */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include "pspdisp.h"

static int fd = -1;
volatile int g_input_enabled = 1;   /* toggled at runtime via SIGUSR1 */

static const struct { uint32_t psp; int key; } map[] = {
  { PSP_CROSS, BTN_SOUTH }, { PSP_CIRCLE, BTN_EAST },
  { PSP_SQUARE, BTN_WEST }, { PSP_TRIANGLE, BTN_NORTH },
  { PSP_LTRIG, BTN_TL }, { PSP_RTRIG, BTN_TR },
  { PSP_START, BTN_START }, { PSP_SELECT, BTN_SELECT },
};
#define NMAP (int)(sizeof(map)/sizeof(map[0]))

static const int keys[] = { BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR,
                            BTN_SELECT, BTN_START };
#define NKEYS (int)(sizeof(keys)/sizeof(keys[0]))

static void absinfo(int code, int min, int max, int flat)
{
  struct uinput_abs_setup a; memset(&a, 0, sizeof a);
  a.code = code; a.absinfo.minimum = min; a.absinfo.maximum = max; a.absinfo.flat = flat;
  ioctl(fd, UI_SET_ABSBIT, code);
  ioctl(fd, UI_ABS_SETUP, &a);
}

bool input_init(void)
{
  fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) { fprintf(stderr, "input: open /dev/uinput failed (udev rule / root)\n"); return false; }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  for (int i = 0; i < NKEYS; i++) ioctl(fd, UI_SET_KEYBIT, keys[i]);

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

void input_update(uint32_t buttons, uint8_t ax, uint8_t ay)
{
  static uint32_t last_keys;
  if (fd < 0) return;

  if (!g_input_enabled) { buttons = 0; ax = ay = 128; }

  /* lol profile: Select is a modifier that turns L/R into the analog triggers
     (LT/RT), giving Q/W/E/R on LT/LB/RB/RT. */
  bool lol = g_opt.profile == PROFILE_LOL;
  bool mod = lol && (buttons & PSP_SELECT);
  uint32_t eff = buttons;
  int lt = 0, rt = 0;
  if (lol) eff &= ~PSP_SELECT;
  if (mod) {
    if (eff & PSP_LTRIG) { lt = 255; eff &= ~PSP_LTRIG; }
    if (eff & PSP_RTRIG) { rt = 255; eff &= ~PSP_RTRIG; }
  }

  for (int i = 0; i < NMAP; i++)
    if ((eff & map[i].psp) != (last_keys & map[i].psp))
      emit(EV_KEY, map[i].key, (eff & map[i].psp) ? 1 : 0);
  last_keys = eff;

  emit(EV_ABS, ABS_X, axis16(ax));
  emit(EV_ABS, ABS_Y, axis16(ay));
  emit(EV_ABS, ABS_Z, lt);
  emit(EV_ABS, ABS_RZ, rt);
  emit(EV_ABS, ABS_HAT0X, ((buttons & PSP_RIGHT) ? 1 : 0) - ((buttons & PSP_LEFT) ? 1 : 0));
  emit(EV_ABS, ABS_HAT0Y, ((buttons & PSP_DOWN) ? 1 : 0) - ((buttons & PSP_UP) ? 1 : 0));
  emit(EV_SYN, SYN_REPORT, 0);
}

void input_term(void)
{
  if (fd >= 0) { ioctl(fd, UI_DEV_DESTROY); close(fd); fd = -1; }
}
