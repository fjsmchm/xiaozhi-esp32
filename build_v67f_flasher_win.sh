#!/bin/bash
# v6.7b 诊断版烧录器打包(Windows Git Bash): 下载CI产物 → base64文件 → 注入v6_6b模板 → 修正版本徽标
set -e
DIR="/e/万银/项目/小智音箱/xiaozhi_flasher_v6_7f"
TPL="/e/万银/项目/小智音箱/xiaozhi_flasher_v6_6b/flasher.html"
mkdir -p "$DIR"
cd /tmp && rm -rf v67bart && mkdir v67bart && cd v67bart

echo "[1/4] 下载CI产物..."
RUNID=$(gh api "repos/fjsmchm/xiaozhi-esp32/actions/runs?branch=ci/ankong-v67&per_page=1" --jq '.workflow_runs[0].id')
gh run download "$RUNID" --repo fjsmchm/xiaozhi-esp32 -p "*ankong-v67*" -D . || gh run download "$RUNID" --repo fjsmchm/xiaozhi-esp32 -D .
BIN=$(find . -name "*merged*.bin" | head -1)
[ -z "$BIN" ] && { echo "未找到merged.bin"; find . -name "*.bin" | head -5; exit 1; }
echo "固件: $BIN ($(wc -c < "$BIN") bytes)"

echo "[2/4] base64编码到临时文件(Windows环境变量有32K限制,必须走文件)..."
base64 -w0 "$BIN" > fw.b64
wc -c < fw.b64

echo "[3/4] 生成烧录器(注入v6_6b模板+改徽标)..."
TPL_WIN=$(cygpath -w "$TPL"); DEST_WIN=$(cygpath -w "$DIR/flasher.html")
TPL_WIN="$TPL_WIN" DEST_WIN="$DEST_WIN" node -e "
const fs=require('fs');
let s=fs.readFileSync(process.env.TPL_WIN,'utf8');
const m=s.match(/var FW64=\"([^\"]*)\"/);
if(!m){console.error('FW64 not found');process.exit(1)}
const b64=fs.readFileSync('fw.b64','utf8').trim();
s=s.replace(m[1], b64);
s=s.replace('固件：小智AI v2.4.2 安控V6.6b（生产版）','固件：小智AI v2.4.2 安控V6.7f（试验4版）');
s=s.replace('本版升级：断线自动重连(5/10/30/60s退避) + 每分钟巡检 + 凌晨4点自愈重启；唤醒词「安控云」','本版升级：自研引擎全配置修正:阈值回0.5+AEC+模型打包+诊断瘦身；旧词安控云不识别');
if(s.indexOf('V6.7f（试验4版）')<0){console.error('徽标替换失败');process.exit(1)}
fs.writeFileSync(process.env.DEST_WIN,s);
console.log('flasher written:',(s.length/1048576).toFixed(1)+'MB');
" || exit 1

echo "[4/4] 生成使用说明..."
cat > "$DIR/使用说明.txt" <<'NOTE'
安控版固件V6.7b（诊断版·自研唤醒引擎）
= V6.6b全部功能 + 唤醒引擎换为自训"安控管家" + 远程诊断上报
用途: 现场实测自研唤醒; 喊"安控管家"时设备会把引擎内部状态
     (喂帧数/峰值置信度/检测次数)经网络上报, 远程即可定位"不唤醒"的病因层
唤醒词: 「安控管家」(旧词"安控云"此版本不识别)
⚠ 若唤醒仍不工作, 无需现场排查——喊几声后联系远程(AI)看日志即可
回退: 烧回 xiaozhi_flasher_v6_6b (生产版)
烧录: Chrome/Edge 本地打开 flasher.html → 连接设备 → 开始烧录(不勾擦除, WiFi保留)
NOTE
echo "完成: $DIR"
