# 构建与发布

需要 JDK 17+、Android SDK 36、Build Tools 36.0.0、NDK 28.2.13676358、CMake 3.22.1。
复制本机 SDK 路径到不受 Git 跟踪的 `local.properties`，例如 `sdk.dir=/path/to/android/sdk`。

## 本地验证

```sh
./gradlew :app:testDebugUnitTest :domain:test :app:assembleDebug -PmihonsyAbi=arm64-v8a
```

测试包位于 `app/build/outputs/apk/debug/`，应用 ID 带 `.debug`，可与正式版共存。
GPU/NPU 推理需要真机验证，JVM 测试覆盖倍率规划、内存边界和版本比较。

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
2. 在中英文 CHANGELOG 中增加对应的 `## vX.Y.Z` 小节。
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
