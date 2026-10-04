/**
 * 设备指纹（对齐 apps/android DeviceIdentity.kt 的角色）：
 * 由稳定种子推导服务端设备台账键（互踢 replacedBy 的展示用设备名另取）。
 * Android 侧为 SHA-256 hex；本侧用 32-bit djb2 双盐折叠成 64-bit hex——
 * 服务端按字符串键使用、不校验摘要算法，语义要求只是「稳定且逐机不同」。
 */
export class DeviceIdentity {
  /** 由稳定种子推导指纹（hex，16 字符）。 */
  static fingerprint(seed: string): string {
    let h1 = 5381;
    let h2 = 52711;
    for (let i = 0; i < seed.length; i++) {
      const c = seed.charCodeAt(i);
      h1 = (((h1 << 5) - h1 + c) | 0) >>> 0; // djb2（32-bit 截断）
      h2 = (((h2 << 5) - h2 + c + i) | 0) >>> 0; // 位置加盐
    }
    return DeviceIdentity.hex32(h1) + DeviceIdentity.hex32(h2);
  }

  private static hex32(v: number): string {
    // v 已是无符号 32-bit；按 8 位十六进制输出
    let s = '';
    for (let i = 7; i >= 0; i--) {
      const nibble = (v >>> (i * 4)) & 0xf;
      s += DeviceIdentity.HEX[nibble];
    }
    return s;
  }

  private static readonly HEX = '0123456789abcdef';
}
