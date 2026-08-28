// 打包V6.6烧录器: 复制v6_5模板 → 替换FW64(base64固件) → 写使用说明
// 用法: node make_v66_flasher.js <merged-binary.bin路径> <输出目录>
const fs = require('fs');
const path = require('path');

const [binPath, outDir] = process.argv.slice(2);
if (!binPath || !outDir) { console.error('用法: node make_v66_flasher.js <bin> <outDir>'); process.exit(1); }

const tplDir = 'E:/万银/项目/小智音箱/xiaozhi_flasher_v6_5';
const bin = fs.readFileSync(binPath);
console.log('固件大小:', bin.length, 'bytes');
const b64 = bin.toString('base64');
console.log('base64长度:', b64.length);

let html = fs.readFileSync(path.join(tplDir, 'flasher.html'), 'utf8');
// FW64="...." 是唯一的巨型base64字面量——用非贪婪定位起点,找结尾引号
const startMarker = 'FW64="';
const si = html.indexOf(startMarker);
if (si < 0) { console.error('未找到FW64定义'); process.exit(1); }
const b64Start = si + startMarker.length;
const b64End = html.indexOf('"', b64Start);
if (b64End < 0 || b64End - b64Start < 1000000) { console.error('FW64内容异常,长度', b64End - b64Start); process.exit(1); }
console.log('原固件base64长度:', b64End - b64Start);
html = html.slice(0, b64Start) + b64 + html.slice(b64End);

fs.mkdirSync(outDir, { recursive: true });
fs.writeFileSync(path.join(outDir, 'flasher.html'), html);
const note = `安控版固件V6.6（常在线自愈版）
= V6.5全部功能(易唤醒) + 两项自愈增强:
  1. 断线自动重连——服务端重启后不再"假在线",推送播报(欢迎回家等)不丢失
  2. 每天04:00自动重启——偶发卡死最多撑到凌晨自愈,无需拔电
烧录: Chrome/Edge 本地打开 flasher.html → 连接设备 → 开始烧录（不勾擦除, WiFi配置保留）
配套: 服务端常在线已开启; 烧完音箱自动重连,无需其他配置
回退: 出问题烧回 xiaozhi_flasher_v6_5`;
fs.writeFileSync(path.join(outDir, '使用说明.txt'), note, 'utf8');
console.log('已生成:', outDir);
