// hvigor 库模块构建脚本（编译腿用；本机无 OHOS SDK，CI 也未跑 ArkTS 编译）。
import { harTasks } from '@ohos/hvigor-ohos-plugin';

export default {
  system: harTasks,
  plugins: []
};