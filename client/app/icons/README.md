# 应用图标资产（A23 图标面）

源＝项目 logo：`docs/src/public/logo.svg`（单路径品牌橙 #e16531）。再生成命令
（ImageMagick 7；仓库不存中间产物，产物全部入库随提交走）：

```bash
cd client/app/icons
for s in 16 32 48 64 128 256 512 1024; do
  magick -background none ../../../docs/src/public/logo.svg \
    -resize ${s}x${s} PNG32:memex-$s.png   # PNG32＝32 位带 alpha（ico 需要真彩色）
done
# Windows 多尺寸 ico（16–256；8bpp 调色板不带 alpha，必须 PNG32 输入）
magick memex-16.png memex-32.png memex-48.png memex-64.png memex-128.png memex-256.png memex.ico
# macOS icns：PNG 载荷手工组包（ic07=128 ic08=256 ic09=512 ic10=1024；
# 本机无 png2icns/iconutil，脚本见下——chunk 头 'icns'＋BE32 总长，每块
# 类型＋BE32(块长+8)＋PNG 字节）
python3 - <<'PY'
import struct
def chunk(t, png): return t + struct.pack('>I', len(png) + 8) + png
parts = b''.join(chunk(t, open(f'memex-{s}.png', 'rb').read())
                 for t, s in ((b'ic07', 128), (b'ic08', 256), (b'ic09', 512), (b'ic10', 1024)))
open('memex.icns', 'wb').write(b'icns' + struct.pack('>I', len(parts) + 8) + parts)
PY
```

消费面（三端同源）：

- `../icons.qrc`：16–512 进 Qt 资源（`:/icons/memex-<N>.png`）——窗口/托盘/
  关于页图标（`brand_icon()` 资源优先，缺失回落程序绘制）；
- `../memex.rc` → `memex.ico`：Windows 可执行内嵌（Explorer/任务栏）＋
  Inno 安装器 `SetupIconFile`（packaging/windows/memex.iss）；
- `memex.icns`：macOS .app bundle（CMake `MACOSX_BUNDLE_ICON_FILE`）；
- `memex-<N>.png`（16–256）：Linux hicolor 安装（deb/rpm
  `/usr/share/icons/hicolor/<N>x<N>/apps/memex.png`，`.desktop` 的 `Icon=memex`
  解析来源；nightly.yml win 暂存面同取 `memex.ico`）；
- 1024 仅作 icns `ic10` 源，不进 qrc。
