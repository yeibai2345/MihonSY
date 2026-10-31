# 构建与发布

需要 JDK 17+、Android SDK 36、Build Tools 36.0.0、NDK 28.2.13676358、CMake 3.22.1。
复制本机 SDK 路径到不受 Git 跟踪的 `local.properties`，例如 `sdk.dir=/path/to/android/sdk`。

## 本地验证

```sh
./gradlew :app:testDebugUnitTest :domain:test :app:assembleDebug -PmihonsyAbi=arm64-v8a
```

测试包位于 `app/build/outputs/apk/debug/`，应用 ID 带 `.debug`，可与正式版共存。
GPU/NPU 推理需要真机验证，JVM 测试覆盖倍率规划、分段边界、动态优先级、取消、内存边界和版本比较。
另运行 `c++ -std=c++17 -O2 -I app/src/main/cpp app/src/test/cpp/upscale_pixels_test.cpp -o /tmp/upscale-pixels-test && /tmp/upscale-pixels-test` 验证像素转换、透明度、边界与纯色缓存。其微基准只测宿主 CPU，不代表手机推理。

## GitHub 正式发布

在本仓库 Actions 中启用工作流。设置以下 Repository secrets：

| Secret | 用途 |
| --- | --- |
| `MIHONSY_KEYSTORE_BASE64` | 正式签名 keystore 的 Base64 内容 |
| `MIHONSY_KEYSTORE_PASSWORD` | keystore 密码 |
| `MIHONSY_KEY_ALIAS` | 可选，默认为 `mihonmod` |
| `MIHONSY_KEY_PASSWORD` | 可选，默认与 keystore 密码相同 |

要覆盖安装现有正式版，必须沿用原签名密钥。请备份密钥，勿提交到仓库。
本地正式构建将 keystore 放在 `keystore/mihonmod.jks`，并设置对应的 `MIHONSY_STORE_PASSWORD`、
`MIHONSY_KEY_PASSWORD` 和可选的 `MIHONSY_KEY_ALIAS` 环境变量。

1. 更新 `app/build.gradle.kts` 中的 `versionName` 和递增的 `versionCode`。
2. 在中英文 CHANGELOG 中增加对应的 `## vX.Y.Z` 小节，并更新 `app/src/main/res/raw/changelog_release.xml` 的应用内公告。
3. 推送代码，手动运行 **Build MihonSY APK**，选择 `formal`。也可在 master 的提交标题中加入 `[release]`。
4. 工作流通过单元测试、构建和签名检查后，会创建 `vX.Y.Z` Release，附上 arm64 APK、universal APK 和 SHA-256 校验文件。

同名标签存在时发布会失败，不会删除或覆盖已发布版本。常规 master/preview 构建只上传测试 APK artifact。
`release` 分支复用同一套正式发布流程；所有构建与发布均留在本仓库。

没有配置 GitHub 签名 Secrets 时，可手动选择 `unsigned`，构建未签名正式包供本地签名；
这不会自动发布 Release。正式 APK 必须签名验证后才能安装或上传给用户。

## AI 倍率与内存保护

GPU 和 NPU 详情页均提供 2x / 3x / 4x / 5x / 6x，与 CPU 插值倍率独立。
内置 2x 网络通过多轮推理实现高倍率；最后一轮之前按目标尺寸重采样，避免产生完整的 8x 中间图。
所有推理输出受 3600 万像素上限约束，过大时逐级降低倍率，2x 仍无法容纳时保留原图。
最终尺寸以阅读器增强状态角标显示的输出分辨率为准。

## 1.3.1 条漫分段与真机验证

非原始尺寸模式下，长宽比 > 2.5、至少 1024 行、通过现有增强尺寸门的长图，可使用支持的 2x 模型分段增强（包括 v75 ESRGAN 模型包）。每段核心最多 512 行，加重叠上下文，末段反射补齐到 8 行边界；每次推理仍受 3600 万像素上限保护。3x 保留 2x → 1.5x → 3x 的真实两轮 AI，4x 保留两轮 2x。

屏幕内区域优先，预取相邻一段；过期工作在排队与推理轮次之间取消，已经进入的原生推理可能需要完成当前轮。原图作为显示底层，完成后叠加增强区域；失败保留原图，重试有上限。16–128 MiB 的会话 LRU 缓存预算仅限制缓存本身，当前显示和推理中的位图另占内存。5x/6x 仍比 3x/4x 更耗内存与算力。

分段角标显示“当前区域”，耗时为覆盖可见区域的已完成分段处理时间之和，含重叠上下文，不是整图时间，也不是首次可见延迟。回看缓存显示原始处理记录。原始尺寸显示、不支持的模型或不满足分段几何条件时沿用整图路径。

发布前自动检查不包含真实 HTP 推理。建议在一加 Ace 5、NPU / ESRGAN 下使用相同章节对比旧版本和 1.3.1 的冷启动、缓存回看、3x/4x 首屏等待、连续滚动、反向回看、跨章、黑白网点和彩色细线接缝、透明 PNG、退出与改倍率。记录原生 QNN 日志中的 prepare/execute/output/cached 以及应用内状态；不得把宿主像素转换微基准描述为整图 NPU 性能。
