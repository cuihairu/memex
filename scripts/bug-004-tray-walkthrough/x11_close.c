// 向 X 窗口发 WM_DELETE_WINDOW（ICCCM 优雅关闭）。
// 走查用：无窗口管理器的裸 Xvfb 上模拟「点关闭按钮」——客户端经
// closeEvent 走 hide-进托盘 路径（BUG-004 场景入口）。
// 用法：DISPLAY=:99 ./x11_close <window-id>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "用法: %s <window-id>\n", argv[0]);
    return 2;
  }
  Display* dpy = XOpenDisplay(NULL);
  if (!dpy) {
    fprintf(stderr, "x11_close: 无法打开显示\n");
    return 1;
  }
  Window w = (Window)strtoul(argv[1], NULL, 0);
  Atom protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
  Atom delete_win = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XClientMessageEvent ev = {0};
  ev.type = ClientMessage;
  ev.window = w;
  ev.message_type = protocols;
  ev.format = 32;
  ev.data.l[0] = delete_win;
  ev.data.l[1] = CurrentTime;
  XSendEvent(dpy, w, False, NoEventMask, (XEvent*)&ev);
  XFlush(dpy);
  return 0;
}
