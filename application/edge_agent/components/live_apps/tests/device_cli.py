from pathlib import Path
import json,re,time,serial,os,codecs
ROOT=Path(__file__).resolve().parents[1]
class Device:
 def __init__(self,port=None):
  self.decoder=codecs.getincrementaldecoder('utf-8')(errors='replace')
  self.serial=serial.Serial(port=None,baudrate=115200,timeout=.1)
  # CLIP_SERIAL_KEEP_RUNNING=1：DTR/RTS 同时拉有效打开串口，自动复位电路不触发，背夹不重启（手机 BLE 不断）
  keep=os.environ.get('CLIP_SERIAL_KEEP_RUNNING')=='1'
  self.serial.dtr=keep;self.serial.rts=keep;self.serial.port=port or os.environ.get('CLIP_SERIAL_PORT','/dev/cu.usbserial-10');self.serial.open()
  self.log=[]
  boot=self.drain(.3);self.serial.write(b'\r\n')
  end=time.monotonic()+55
  while time.monotonic()<end and 'app> ' not in boot:
   boot+=self.drain(.3)
  self.log.append(boot);self.drain(.5)
  if 'app> ' not in boot:raise RuntimeError('CLI not ready')
 def close(self):self.serial.close()
 def drain(self,seconds):
  end=time.monotonic()+seconds;b=bytearray()
  while time.monotonic()<end:
   v=self.serial.read(self.serial.in_waiting or 1)
   if v:b.extend(v)
  return self.decoder.decode(bytes(b),final=False)
 def send(self,command,timeout=15):
  self.serial.reset_input_buffer();payload=(command+'\r\n').encode()
  # The UART REPL echoes input and has a small RX ring. Pace large test JSON;
  # production Bluetooth packets use the separate framed transport and CRC.
  for offset in range(0,len(payload),64):
   self.serial.write(payload[offset:offset+64]);self.serial.flush()
   if len(payload)>128:time.sleep(.02)
  text='';end=time.monotonic()+timeout
  while time.monotonic()<end:
   text+=self.drain(.15)
   if 'CLIP_RESULT ' in text:
    # Wait for the response newline: Lua source contains its own braces and a
    # partial UART read can end at one of those before the outer JSON ends.
    match=re.search(r'CLIP_RESULT (\{[^\r\n]*\})\r?\n',text)
    if match:
     result=json.loads(match.group(1));self.log.append(text);return result
  self.log.append(text);raise RuntimeError('No CLI response: '+text[-800:])
 def rpc(self,body):
  # IDF's dumb console filters signed non-ASCII input bytes. JSON Unicode
  # escapes preserve the exact UTF-8 payload after cJSON decoding.
  raw=json.dumps(body,ensure_ascii=True,separators=(',',':'))
  escaped='"'+raw.replace('\\','\\\\').replace('"','\\"')+'"'
  result=self.send('clip_cmd '+escaped)
  if not result.get('ok'):raise RuntimeError(str(result))
  return result
