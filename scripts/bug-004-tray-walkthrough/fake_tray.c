// 极简 X11 系统托盘宿主（freedesktop System Tray Protocol v1，XEmbed 嵌入）。
// 用途：无桌面环境（裸 Xvfb）下为 BUG-004 走查提供可真实点击的托盘图标——
// 占据 _NET_SYSTEM_TRAY_S0 selection 并广播 MANAGER，收到 dock 请求后把
// Qt 托盘图标窗口 reparent 进宿主窗口并 map，再按 XEmbed 规范发
// EMBEDDED_NOTIFY。仅为输入面走查，不实现托盘 UI（背景/布局/气泡）。
//
// 构建：gcc fake_tray.c -o fake_tray -lX11
// 运行：DISPLAY=:99 ./fake_tray [x y]   # 缺省 (1200, 900)
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  const int x = argc > 1 ? atoi(argv[1]) : 1200;
  const int y = argc > 2 ? atoi(argv[2]) : 900;
  Display* dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "fake-tray: 无法打开显示\n");
    return 1;
  }
  const int scr = DefaultScreen(dpy);
  Window root = RootWindow(dpy, scr);
  // 40x40 宿主窗口；图标嵌入在 (8,8)，点击目标取宿主 (x+16, y+16)
  Window win = XCreateSimpleWindow(dpy, root, x, y, 40, 40, 1,
                                   BlackPixel(dpy, scr),
                                   WhitePixel(dpy, scr));
  XStoreName(dpy, win, "memex-fake-tray");
  XSelectInput(dpy, win, StructureNotifyMask);

  Atom sel = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
  Atom manager = XInternAtom(dpy, "MANAGER", False);
  Atom opcode = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
  Atom xembed = XInternAtom(dpy, "_XEMBED", False);

  // 成为托盘宿主并广播（已在跑的应用靠 MANAGER 消息感知托盘出现）
  XSetSelectionOwner(dpy, sel, win, CurrentTime);
  XClientMessageEvent ann = {0};
  ann.type = ClientMessage;
  ann.window = root;
  ann.message_type = manager;
  ann.format = 32;
  ann.data.l[0] = CurrentTime;
  ann.data.l[1] = win;
  ann.data.l[2] = sel;
  XSendEvent(dpy, root, False, StructureNotifyMask, (XEvent*)&ann);
  XMapWindow(dpy, win);
  XSync(dpy, False);
  fprintf(stderr, "fake-tray: 就绪 @(%d,%d) win=0x%lx\n", x, y, (unsigned long)win);

  XEvent ev;
  while (1) {
    XNextEvent(dpy, &ev);
    if (ev.type == ClientMessage && ev.xclient.message_type == opcode &&
        ev.xclient.data.l[1] == 0 /* SYSTEM_TRAY_REQUEST_DOCK */) {
      Window icon = (Window)ev.xclient.data.l[2];
      XReparentWindow(dpy, icon, win, 8, 8);
      XMapWindow(dpy, icon);
      // XEmbed 嵌入通告（embedder→client）：l[0]=时间戳 l[1]=EMBEDDED_NOTIFY
      // l[2]=embedder 版本 l[3]=0
      XClientMessageEvent notify = {0};
      notify.type = ClientMessage;
      notify.window = icon;
      notify.message_type = xembed;
      notify.format = 32;
      notify.data.l[0] = CurrentTime;
      notify.data.l[1] = 0; /* XEMBED_EMBEDDED_NOTIFY */
      notify.data.l[2] = 0; /* 版本 */
      notify.data.l[3] = 0;
      XSendEvent(dpy, icon, False, NoEventMask, (XEvent*)&notify);
      XSync(dpy, False);
      fprintf(stderr, "fake-tray: 已嵌入图标 0x%lx\n", (unsigned long)icon);
    }
  }
}
