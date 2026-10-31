# 构建与发布

需要 JDK 17+、Android SDK 36、Build Tools 36.0.0、NDK 28.2.13676358、CMake 3.22.1。
复制本机 SDK 路径到不受 Git 跟踪的 `local.properties`，例如 `sdk.dir=/path/to/android/sdk`。

## 本地验证

```sh
./gradlew :app:testDebugUnitTest :domain:test :app:assembleDebug -PmihonsyAbi=arm64-v8a
```

测试包位于 `app/build/outputs/apk/debug/`，应用 ID 带 `.debug`，可与正式版共存。
GPU/NPU 推理需要真机验证，JVM 测试覆盖倍率规划、温控准入、动态优先级、取消、内存边界和版本比较。
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

## 1.3.2 整图显示、温控与真机验证

恢复 1.2.1 的整图增强后显示方式。3x 保留 2x → 1.5x → 3x 的真实两轮 AI，4x 保留两轮 2x；超大图仍按上述 3600 万像素上限降倍率。移除 1.3.1 的原图预览和分段叠层。

可见页优先，只额外预取阅读方向相邻一页；跨章首页仍有优先级。滚动或热状态高于 NONE 时暂停新的离屏 AI，CRITICAL 及以上暂停新推理。远处的压缩源等待准入后才提交 Coil，已经取消的原生任务在 tile 边界退出。

NPU 不再在模型初始化时常驻最高性能：推理期间使用独立 QNN 功耗请求，开启 DCVS 和休眠，关闭 RPC 忙轮询。冷机请求 NOM_PLUS、上限 TURBO；LIGHT 上限 NOM_PLUS、MODERATE 上限 NOM、SEVERE 上限 SVS_PLUS。CRITICAL 暂停并释放请求，降温后继续；结束、失败和取消均释放请求。QNN 拒绝性能提示时使用系统策略，并记录一次诊断。温控来自 Android 10+ 的系统热状态，不等于摄氏温度，也不保证每台设备报告及时。旧系统保留平衡功耗策略。

角标记录整张图的解码与增强处理，剔除调度等待；正在推理期间遇到的临界温控等待仍计入。回看缓存保留原始处理记录。1.3.1 的“当前区域”角标与本版整图不是同一工作量，不能直接比较。原生 QNN 日志包含 prepare/execute/output/cached/thermal；execute 包含 tile 内遇到的降温等待。

宿主检查还包括：

```sh
c++ -std=c++17 -O2 -pthread -I app/src/main/cpp -I app/src/main/cpp/include/QNN app/src/test/cpp/inference_control_test.cpp -o /tmp/inference-control-test
/tmp/inference-control-test
```

发布前自动检查不包含真实 HTP 推理。一加 Ace 5 / NPU / ESRGAN 应使用相同源图、倍率、设置、亮度及初始温度，分别记录 3x/4x 冷机整图、连续阅读 10–15 分钟、缓存回看、跨章、快速滚动后返回、退出与改倍率。对比最终尺寸和画质，同时记录实际等待、角标耗时、温控等级与机身温度。降低功耗请求可能牺牲单张峰值速度，应以持续阅读数据评估；不得把宿主像素微基准当成手机整图 NPU 性能。
