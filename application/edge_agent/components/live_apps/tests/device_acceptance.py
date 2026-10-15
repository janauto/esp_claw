from pathlib import Path
import sys, json, hashlib, time, base64, struct, zlib, importlib.util
import argparse
parser=argparse.ArgumentParser(description='Read back sources/settings and run all three live apps on the device')
parser.add_argument('--before',required=True,type=Path)
parser.add_argument('--out',required=True,type=Path)
parser.add_argument('--port',default='/dev/cu.usbserial-10')
args=parser.parse_args()
root=Path(__file__).resolve().parent
out=args.out;out.mkdir(parents=True,exist_ok=True)
before=json.loads(args.before.read_text())
spec=importlib.util.spec_from_file_location('device_cli',root/'device_cli.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def png(path,w,h,pixels):
 raw=bytearray()
 for y in range(h):
  raw.append(0)
  for x in range(w):
   v=struct.unpack_from('<H',pixels,(y*w+x)*2)[0];raw.extend(((v>>11)*255//31,((v>>5)&63)*255//63,(v&31)*255//31))
 def chunk(k,d):return struct.pack('>I',len(d))+k+d+struct.pack('>I',zlib.crc32(k+d)&0xffffffff)
 path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(raw))+chunk(b'IEND',b''))
d=m.Device(args.port);report={}
try:
 report['device_info']=d.rpc({'op':'device_info'});assert report['device_info']['protocol']=='esp-claw-ble-live-apps'
 assert report['device_info']['version']=='ble-live-1'
 report['app_list_before']=d.rpc({'op':'app_list'})
 print('VERSION',report['device_info']['version'],flush=True)
 print('APPS',json.dumps(report['app_list_before'],ensure_ascii=False),flush=True)
 report['apps']={}
 for app in ['live_weather','live_aihot','live_stocks']:
  item={};report['apps'][app]=item
  package=d.rpc({'op':'app_get','id':app});(out/(app+'-readback.json')).write_text(json.dumps(package,ensure_ascii=False,indent=2))
  src=package['source'].encode();item['source_sha256']=hashlib.sha256(src).hexdigest();item['source_bytes']=len(src);item['package']=package.get('package')
  expected=before['apps'][app]['source'].encode();item['matches_before_source']=src==expected
  assert item['matches_before_source'], app+' SOURCE CHANGED'
  item['start']=d.rpc({'op':'app_run','id':app});time.sleep(2)
  item['ui']=d.rpc({'op':'live_ui_state'})
  assert item['ui']['ui']['view']=={'live_weather':'weather','live_aihot':'aihot','live_stocks':'stocks'}[app]
  capture=d.rpc({'op':'screen_capture','scale':1,'encoding':'rle'});item['capture']=capture;encoded=bytearray()
  for off in range(0,int(capture['bytes']),4096):
   r=d.rpc({'op':'screen_read','capture_id':capture['capture_id'],'offset':off,'max_bytes':4096});encoded.extend(base64.b64decode(r['data']))
  pixels=bytearray()
  if capture['format']=='rle565le':
   for n,value in struct.iter_unpack('<HH',encoded):pixels.extend(struct.pack('<H',value)*n)
  else:pixels=encoded
  assert len(pixels)==int(capture['width'])*int(capture['height'])*2
  png(out/(app+'.png'),int(capture['width']),int(capture['height']),pixels)
  d.rpc({'op':'screen_release'});item['stop']=d.rpc({'op':'app_stop'})
  print('APP_VERIFIED',app,json.dumps({k:v for k,v in item.items() if k in ['source_sha256','source_bytes','matches_before_source','ui']},ensure_ascii=False),flush=True)
 report['app_list_after']=d.rpc({'op':'app_list'});report['device_info_after']=d.rpc({'op':'device_info'})
 assert report['app_list_before']==report['app_list_after']
 expected_ids={a['id'] for a in before['app_list']['apps'] if not a.get('builtin')}
 actual_ids={a['id'] for a in report['app_list_after']['apps']}
 assert expected_ids.issubset(actual_ids), expected_ids-actual_ids
 oldnet=before['device_info']['network'];newnet=report['device_info_after']['network']
 for key in ['city','latitude','longitude','symbols','mode','weatherDefaultId','weatherLocations']:
  if key in oldnet: assert oldnet[key]==newnet[key], 'SETTING CHANGED '+key
 report['settings_preserved']=True
 report['installed_apps_preserved']=True
 print('PASS: sources, installed apps, settings and three device UI captures',flush=True)
finally:
 d.close();(out/'app-verification.json').write_text(json.dumps(report,ensure_ascii=False,indent=2));(out/'verification-uart.log').write_text('\n'.join(d.log))
