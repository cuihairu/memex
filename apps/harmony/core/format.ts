/**
 * 通知三级与通知正文归档形态（镜像 apps/android NoticeGrade.kt，对齐
 * 桌面 T4.10 / proto Notice.Urgency 语义）。
 */

export enum NoticeGrade {
  NORMAL = 'NORMAL',
  IMPORTANT = 'IMPORTANT',
  URGENT = 'URGENT',
}

/** proto Notice.Urgency 编号 → 分级（未指定/未识别按普通，对齐桌面）。 */
export function fromProtoNumber(n: number): NoticeGrade {
  if (n === 2) return NoticeGrade.IMPORTANT;
  if (n === 3) return NoticeGrade.URGENT;
  return NoticeGrade.NORMAL;
}

/**
 * 通知正文的归档形态（对齐桌面 compose_notice_text，服务端归档同源生成）：
 * 「标题：正文[ 跳转]」（全角冒号 U+FF1A），气泡单行渲染，跳转随文留痕。
 */
export function composeNoticeText(title: string, content: string, jumpUrl: string): string {
  let s = title + '：' + content;
  if (jumpUrl.length > 0) s += ' ' + jumpUrl;
  return s;
}