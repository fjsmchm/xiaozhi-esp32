# -*- coding: utf-8 -*-
"""ANKONG V4 服务端补丁：连接注册表 + /api/broadcast 播报接口 + 常在线配置"""
import shutil, datetime, py_compile

ts = datetime.datetime.now().strftime('%Y%m%d%H%M')
base = '/opt/xiaozhi-esp32-server/'

# 1) connection.py: 广播注册表
p = base + 'core/connection.py'
shutil.copy(p, p + '.bak.' + ts)
s = open(p, encoding='utf-8').read()
assert '_BROADCAST_CONNS' not in s, 'already patched'
s = s.replace('class ConnectionHandler:',
              '# ANKONG-PATCH: 按device_id登记在线连接,供/api/broadcast播报\n_BROADCAST_CONNS={}\n\n\nclass ConnectionHandler:', 1)
a = '            self.websocket = ws'
assert s.count(a) == 1, 'ws anchor not unique: %d' % s.count(a)
s = s.replace(a, a + '\n            # ANKONG-PATCH: 登记在线连接\n            if self.device_id:\n                _BROADCAST_CONNS[self.device_id]=self', 1)
a2 = '    async def close(self, ws=None):'
assert a2 in s, 'close anchor missing'
s = s.replace(a2, a2 + '\n        # ANKONG-PATCH: 注销广播注册\n        if getattr(self,\'device_id\',None) and _BROADCAST_CONNS.get(self.device_id) is self:\n            _BROADCAST_CONNS.pop(self.device_id,None)', 1)
open(p, 'w', encoding='utf-8').write(s)
py_compile.compile(p, doraise=True)
print('connection.py OK')

# 2) http_server.py: /api/broadcast 路由+处理
p2 = base + 'core/http_server.py'
shutil.copy(p2, p2 + '.bak.' + ts)
h = open(p2, encoding='utf-8').read()
assert 'api/broadcast' not in h, 'already patched'
route_anchor = '''                        web.options(
                            "/mcp/vision/explain", self.vision_handler.handle_options
                        ),'''
assert route_anchor in h, 'route anchor missing'
h = h.replace(route_anchor, route_anchor + '''
                        web.post("/api/broadcast", self.handle_broadcast),''', 1)

handler = '''
    async def handle_broadcast(self, request):
        """ANKONG-PATCH: 播报接口——向指定在线音箱推一段语音(走TTS流水线)"""
        import uuid
        import aiohttp
        from aiohttp import web
        from core.connection import _BROADCAST_CONNS
        from core.providers.tts.dto.dto import ContentType, TTSMessageDTO, SentenceType
        try:
            data = await request.json()
            device_id = str(data.get("device_id", ""))
            text = str(data.get("text", "")).strip()
            key = request.headers.get("X-AK-Key", "")
            from plugins_func.functions.ankong_control import ANKONG_KEY
            if not device_id or not text:
                return web.json_response({"code": 1, "message": "params"})
            if key != ANKONG_KEY:
                return web.json_response({"code": 2, "message": "unauthorized"})
            conn = _BROADCAST_CONNS.get(device_id)
            if not conn:
                return web.json_response({"code": 3, "message": "device offline"})
            if conn.tts is None:
                conn.tts = conn._initialize_tts()
            sid = uuid.uuid4().hex
            conn.sentence_id = sid
            conn.client_abort = False
            conn.tts.tts_text_queue.put(
                TTSMessageDTO(sentence_id=sid, sentence_type=SentenceType.FIRST,
                              content_type=ContentType.TEXT, content_detail=text))
            conn.tts.tts_text_queue.put(
                TTSMessageDTO(sentence_id=sid, sentence_type=SentenceType.LAST,
                              content_type=ContentType.TEXT, content_detail=None))
            self.logger.bind(tag="BROADCAST").info(f"播报已入队 {device_id}: {text}")
            return web.json_response({"code": 0, "message": "ok"})
        except Exception as e:
            return __import__("aiohttp").web.json_response({"code": 9, "message": str(e)})

'''
anchor2 = '    def _get_websocket_url(self, local_ip: str, port: int) -> str:'
assert anchor2 in h, 'handler anchor missing'
h = h.replace(anchor2, handler + anchor2, 1)
open(p2, 'w', encoding='utf-8').write(h)
py_compile.compile(p2, doraise=True)
print('http_server.py OK')

# 3) 常在线: 空闲7天才断
p3 = base + 'data/.config.yaml'
c = open(p3, encoding='utf-8').read()
if 'close_connection_no_voice_time' not in c:
    c = c.replace('  timezone_offset: +8', '  timezone_offset: +8\n  close_connection_no_voice_time: 604800', 1)
    open(p3, 'w', encoding='utf-8').write(c)
print('keep-alive OK (604800s)')
