// V4 烧录器打包：以 V2 flasher.html 为模板，替换内嵌固件(FW64=base64(merged.bin))与文案
// 用法: node _pack_flasher.js <merged.bin> <输出flasher.html>
const fs = require('fs');
const [binPath, outPath] = process.argv.slice(2);
if (!binPath || !outPath) { console.error('用法: node _pack_flasher.js <merged.bin> <out.html>'); process.exit(1); }

const TEMPLATE = 'C:/Users/chm/Desktop/xiaozhi_flasher_ankong/flasher.html';
const tpl = fs.readFileSync(TEMPLATE, 'utf8');

const bin = fs.readFileSync(binPath);
if (bin[0] !== 0xE9) { console.error('不是ESP镜像(缺E9头):', bin[0]); process.exit(1); }
const b64 = bin.toString('base64');

const m = tpl.match(/var FW64="([A-Za-z0-9+/=]+)";/);
if (!m) { console.error('模板中未找到 FW64'); process.exit(1); }
const v2bin = Buffer.from(m[1], 'base64');
console.log('V2载荷:', (v2bin.length/1048576).toFixed(2)+'MB 头:'+v2bin.slice(0,1).toString('hex'));

const rt = Buffer.from(b64, 'base64');
if (!rt.equals(bin)) { console.error('回环校验失败'); process.exit(1); }
console.log('新固件:', (bin.length/1048576).toFixed(2)+'MB → base64', (b64.length/1048576).toFixed(1)+'MB 回环OK');

let out = tpl.replace(/var FW64="[A-Za-z0-9+/=]+";/, 'var FW64="' + b64 + '";');
out = out.replace('固件：小智AI v2.4.2 官方版', '固件：小智AI v2.4.2 安控V4');
out = out.replace(/大小：[\d.]+ MB/, '大小：' + (bin.length/1048576).toFixed(1) + ' MB');
fs.writeFileSync(outPath, out);
console.log('已输出:', outPath, (fs.statSync(outPath).size/1048576).toFixed(1)+'MB');
