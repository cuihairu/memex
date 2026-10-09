# 内置默认头像组（需求批⑫）

四张纯几何自绘（无任何外部素材），品牌橙 `#e16531`（与 logo 同源）衍生配色；
未设置头像的账号按 `FNV-1a(UTF-8(account)) % 4` 稳定取一张（同账号跨端一致），
客户端引用 `:/avatars/avatar-<N>.png`（见 `avatars.qrc`）。256×256 单尺寸入库，
小尺寸由消费面平滑缩放。

再生成命令（ImageMagick 7；仓库不存中间产物，产物全部入库随提交走）：

```bash
cd client/app/icons/avatars
# 1 橙圆（浅底 accent 圆）
magick -size 256x256 xc:none -fill "#f9e7dc" -draw "roundrectangle 0,0 255,255 48,48" \
  -fill "#e16531" -draw "circle 128,128 128,52" PNG32:avatar-1.png
# 2 同心环（accent 底白环白点）
magick -size 256x256 xc:none -fill "#e16531" -draw "roundrectangle 0,0 255,255 48,48" \
  -fill none -stroke "#fff6f0" -strokewidth 18 -draw "circle 128,128 128,58" \
  -fill "#fff6f0" -draw "circle 128,128 128,104" PNG32:avatar-2.png
# 3 三角（深灰底 accent 三角）
magick -size 256x256 xc:none -fill "#33302e" -draw "roundrectangle 0,0 255,255 48,48" \
  -fill "#e16531" -draw "polygon 128,56 212,200 44,200" PNG32:avatar-3.png
# 4 三竖条（浅底三档 accent 圆角条）
magick -size 256x256 xc:none -fill "#f9e7dc" -draw "roundrectangle 0,0 255,255 48,48" \
  -fill "#e16531" -draw "roundrectangle 64,72 92,184 14,14" \
  -fill "#ec8b5c" -draw "roundrectangle 114,72 142,184 14,14" \
  -fill "#f3b48c" -draw "roundrectangle 164,72 192,184 14,14" PNG32:avatar-4.png
```
