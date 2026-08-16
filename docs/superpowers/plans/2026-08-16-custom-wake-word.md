# 小智自定义唤醒词「你好安控」实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 Waveshare ESP32-S3-AUDIO-Board 编译一份唤醒词为「你好安控」的小智固件（基于官方 v2.4.2 + MultiNet），并集成到现有烧录器供用户刷入。

**Architecture:** 官方 v2.4.2 已内置 MultiNet 唤醒实现（`custom_wake_word.cc`，Kconfig 驱动，无需改 C++ 代码）。在 fork 仓库给板卡 config.json 新增一个 build variant（sdkconfig_append 注入拼音/阈值/分区表/MultiNet 模型），GitHub Actions 云端编译出 merged-binary.bin，替换烧录器内置固件。

**Tech Stack:** ESP-IDF v6.0.2（CI 容器）、esp-sr ~2.4.7（MultiNet mn7_cn）、GitHub Actions、esptool-js 烧录器（已建）。

## Global Constraints

- 固件基线 = 官方 tag **v2.4.2**（与板子现刷版本一致，OTA 指向 `http://159.75.91.11:8103/xiaozhi/ota/` 不变）
- fork 仓库：`fjsmchm/xiaozhi-esp32`，本地 `C:/Users/chm/ZCodeProject/xiaozhi-fw`
- 板卡目录：`main/boards/waveshare/esp32-s3-audio-board/`
- 不修改任何 .cc/.h；只改 config.json（新增 variant，不动原 variant）
- 唤醒词拼音：`ni hao an kong`；显示名 `你好安控`；初始阈值 20（1-99，越小越灵敏）
- MultiNet 模型 Kconfig：`CONFIG_SR_MN_CN_MULTINET7_QUANT=y`（mn7_cn，来源 esp-sr Kconfig.projbuild L346）
- 分区表：`partitions/v1/16m_custom_wakeword.csv`（model 分区 0x3f0000，装得下 mn7 模型）
- CI：`.github/workflows/build.yml` 由 push 到 `ci/*` 分支触发，产物 zip 含 merged-binary.bin
- 验证规则（CLAUDE.md）：不动线上安控云/小智容器；烧录器 zip 更新后浏览器可下载

---

### Task 1: 新增板卡 build variant

**Files:**
- Modify: `main/boards/waveshare/esp32-s3-audio-board/config.json`

**Interfaces:**
- Produces: build variant 名 `esp32-s3-audio-board-ankong`（OTA 上报的 board name），CI 产物 `esp32-s3-audio-board-ankong.zip`

- [ ] **Step 1: 编辑 config.json，builds 数组追加新条目**

```json
{
    "name": "esp32-s3-audio-board-ankong",
    "sdkconfig_append": [
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\"partitions/v1/16m_custom_wakeword.csv\"",
        "CONFIG_USE_CUSTOM_WAKE_WORD=y",
        "CONFIG_CUSTOM_WAKE_WORD=\"ni hao an kong\"",
        "CONFIG_CUSTOM_WAKE_WORD_DISPLAY=\"你好安控\"",
        "CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20",
        "CONFIG_SR_MN_CN_MULTINET7_QUANT=y"
    ]
}
```

（原 `esp32-s3-audio-board` 条目保持不变）

- [ ] **Step 2: 本地校验 JSON 合法**

Run: `node -e "JSON.parse(require('fs').readFileSync('main/boards/waveshare/esp32-s3-audio-board/config.json'));console.log('JSON OK')"`
Expected: `JSON OK`

- [ ] **Step 3: 提交并推送 ci 分支**

```bash
git checkout -b ci/ankong-wake-word
git add main/boards/waveshare/esp32-s3-audio-board/config.json
git commit -m "feat(waveshare): add ankong custom wake word variant (ni hao an kong)"
git push origin ci/ankong-wake-word
```

### Task 2: CI 编译与产物获取

**Files:**
- 无代码修改（使用现有 build.yml）

**Interfaces:**
- Consumes: Task 1 的 ci/ankong-wake-word 分支
- Produces: `v2.4.2-ankong_esp32-s3-audio-board-ankong.zip`（内含 merged-binary.bin）

- [ ] **Step 1: 确认 Actions 触发**

Run: `gh run list --repo fjsmchm/xiaozhi-esp32 --limit 3`
Expected: 看到 `Build Boards` workflow 对 ci/ankong-wake-word 分支 queued/in_progress

- [ ] **Step 2: 等待编译完成（预计 15-40 分钟）**

Run: `gh run watch --repo fjsmchm/xiaozhi-esp32 --exit-status`（或轮询 `gh run list`）
Expected: conclusion=success

- [ ] **Step 3: 失败时的排查点（按序检查）**

1. prepare job 的 variants 列表是否含 `waveshare/esp32-s3-audio-board-ankong`（config.json 解析）
2. 编译日志中 `CustomWakeWord` 相关 Kconfig 是否生效（grep `CONFIG_USE_CUSTOM_WAKE_WORD=y`）
3. 若 sdkconfig 报 unknown symbol：检查 esp-sr 2.4.7 实际符号名（下载组件 zip 比对 Kconfig），修正后重推

- [ ] **Step 4: 下载产物并校验**

Run: `gh run download --repo fjsmchm/xiaozhi-esp32 -n <artifact名> -D /tmp/xz_fw_custom && ls -la`
Expected: zip 内 merged-binary.bin 存在，大小 4-13MB

### Task 3: 烧录器集成

**Files:**
- Modify: 服务器 `/tmp/xz_fw/assemble.py` 输入（merged-binary.bin 换为新固件）
- Produces: `http://159.75.91.11:8000/s/xiaozhi_flasher_ankong.zip`

**Interfaces:**
- Consumes: Task 2 的 merged-binary.bin
- Produces: 新烧录器 zip（单文件含新固件，用法与旧版一致）

- [ ] **Step 1: 上传固件到服务器并重组装**

```bash
scp <merged-binary.bin> ubuntu@159.75.91.11:/tmp/xz_fw/merged-binary-ankong.bin
# 服务器上: cp merged-binary-ankong.bin merged-binary.bin && python3 assemble.py && 打zip
```

- [ ] **Step 2: 部署 + 外网可下载验证**

Run: `curl -sI http://159.75.91.11:8000/s/xiaozhi_flasher_ankong.zip | head -1`
Expected: `HTTP/1.1 200 OK`

### Task 4: 用户刷入与效果验证

- [ ] **Step 1: 用户用烧录器刷入（擦除+烧录）**
- [ ] **Step 2: 板子重启后连回自建服务器（OTA 地址在 NVS，可能需重配网）**
- [ ] **Step 3: 喊「你好安控」验证唤醒；不灵则调 `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD`（更小更灵敏）重编译**
- [ ] **Step 4: 「你好小智」应不再唤醒（MultiNet 替换了 WakeNet）**

### 风险与回退

- 灵敏度不如 WakeNet（MultiNet 特性）：阈值可调；不满意可刷回官方固件（`xiaozhi_flasher.zip` 保留）
- 配网信息可能丢失（分区表变了）：重走配网，OTA 地址重新填一次
- esp-sr 2.4.7 符号名可能与 master 不同：Task 2 Step 3 有排查路径

<!-- trigger 1786887981197 -->
