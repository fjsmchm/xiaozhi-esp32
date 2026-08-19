#!/bin/bash
# V6烧录器打包:下载CI产物merged.bin → base64 → 换入V4烧录器模板
set -e
DIR="$HOME/Desktop/xiaozhi_flasher_v6"
mkdir -p "$DIR"
cd /tmp && rm -rf v6art && mkdir v6art && cd v6art
echo "[1/4] 下载CI产物..."
gh run download --repo fjsmchm/xiaozhi-esp32 --branch ci/ankong-v6 --name "$(gh run list --repo fjsmchm/xiaozhi-esp32 --branch ci/ankong-v6 --limit 1 --json displayTitle --jq '.[0].displayTitle' >/dev/null 2>&1; gh run list --repo fjsmchm/xiaozhi-esp32 --branch ci/ankong-v6 --limit 1 --json databaseId --jq '.[0].databaseId' | xargs -I{} gh api repos/fjsmchm/xiaozhi-esp32/actions/runs/{}/artifacts --jq '.artifacts[0].name')" . 2>/dev/null || gh run download --repo fjsmchm/xiaozhi-esp32 --branch ci/ankong-v6 .
BIN=$(find . -name "*ankong*merged*.bin" -o -name "merged.bin" | head -1)
[ -z "$BIN" ] && { echo "未找到merged.bin"; find . -name "*.bin" | head -5; exit 1; }
echo "固件: $BIN ($(stat -c%s "$BIN") bytes)"
echo "[2/4] base64编码..."
B64=$(base64 -w0 "$BIN")
echo "[3/4] 生成烧录器..."
node -e "
const fs=require('fs');
let s=fs.readFileSync(process.env.HOME+'/Desktop/xiaozhi_flasher_v4/flasher.html','utf8');
const m=s.match(/var FW64=\"([^\"]*)\"/);
if(!m){console.error('FW64 not found');process.exit(1)}
s=s.replace(m[1], process.argv[1]);
s=s.split('安控版固件V4').join('安控版固件V6').split('V4 ').join('V6 ').split('V4\u00b7').join('V6\u00b7');
s=s.split('V4:').join('V6:').split('(V4').join('(V6');
fs.writeFileSync(process.env.DEST,s);
console.log('flasher written:',(s.length/1048576).toFixed(1)+'MB');
" "$B64"
echo "[4/4] 生成使用说明..."
cat > "$DIR/使用说明.txt" <<'NOTE'
安控版固件V6 烧录说明（常在线播报）

V6 = V4全部功能 + 常在线播报:
1. 短唤醒词: 安控云 / 安心居 (不带"你好"也行), 阈值15
2. WiFi 永不休眠
3. 常在线播报: 对话结束2分钟后音箱回待机(唤醒词正常工作),
   但连接保持——服务器随时可推送播报(如开门"欢迎回家"), 无需唤醒音箱

原理: 连接保持与对话状态分离。唤醒词仍是唯一对话入口, 不会一直对话。

烧录步骤:
1. Chrome/Edge 打开 flasher.html（本地打开）
2. USB-C 连板子 -> 连接设备 -> 开始烧录（勾选擦除）
3. 刷完重新配网: 热点 Xiaozhi-XXXX -> 192.168.4.1 -> 选 WiFi -> OTA地址: http://159.75.91.11:8103/xiaozhi/ota/
4. 喊「安控云」唤醒

验证常在线: 唤醒说一句话 -> 等2分钟以上(音箱安静待机) -> 喊「安控云」应能唤醒(V5问题已修) -> 再等2分钟 -> 服务器发播报, 音箱应直接说出"欢迎回家"
NOTE
echo "完成: $DIR"
