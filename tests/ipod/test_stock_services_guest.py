#!/usr/bin/env python3
"""Native 7E18 Weather/Stocks XML acceptance with local synthetic data only."""
import sys,time,tempfile,json,threading,plistlib,argparse
parser=argparse.ArgumentParser(description=__doc__)
group=parser.add_mutually_exclusive_group()
group.add_argument("--stocks",action="store_true")
group.add_argument("--live-weather",action="store_true",help="contact Open-Meteo; verify search and Fahrenheit/Celsius forecasts")
options=parser.parse_args()
from pathlib import Path
from types import SimpleNamespace
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path(__file__).resolve().parents[2];sys.path.insert(0,str(root/'tests/ipod'));import regress as r
out=Path(tempfile.mkdtemp(prefix='it-weather-protocol-'));print('OUTPUT',out,flush=True)
r.START=time.time();files=str(root.parent/'qemu-ios-files')
routing=out/'proxy.conf';requests=[]
class Handler(BaseHTTPRequestHandler):
 def capture(self):
  length=int(self.headers.get('Content-Length','0'))
  if length>1024*1024:self.send_error(413);return
  body=self.rfile.read(length)
  from urllib.parse import urlsplit,parse_qs
  target=urlsplit(self.path)
  # Never retain the gateway's per-install UUID or request headers.
  requests.append(dict(method=self.command,host=target.hostname,path=target.path,service=parse_qs(target.query).get('apptype',[]),body=body.decode('utf-8','replace')))
  (out/'requests.json').write_text(json.dumps(requests,indent=2))
  response=b'';code=410
  if self.command=='POST' and 'apptype=weather' in self.path:
   import xml.etree.ElementTree as ET
   request=ET.fromstring(body);response_root=ET.Element('response');listing=ET.SubElement(ET.SubElement(response_root,'result'),'list')
   if request.find('./query').get('type')=='getlocationid':
    item=ET.SubElement(listing,'item')
    for key,value in {'id':'ltm:37.323,-122.032:Fixture','city':'Fixture City','region':'CA','regionname':'California','country':'US','countryname':'United States'}.items():ET.SubElement(item,key).text=value
   for location in request.findall('./query/list/id'):
    item=ET.SubElement(listing,'item')
    ET.SubElement(item,'location',id=location.text,city='Fixture City')
    ET.SubElement(item,'units',temperature='F')
    ET.SubElement(item,'astronomy',sunrise='06:00',sunset='20:00',moonfacevisible='0.5',moonphase='0')
    ET.SubElement(item,'condition',time='14:30',temp='73',code='32')
    for day in range(6):ET.SubElement(item,'forecast',high=str(80+day),low=str(60+day),code='32',dayofweek=str(day+1))
   response=ET.tostring(response_root,encoding='utf-8',xml_declaration=True);code=200
  if self.command=='POST' and 'apptype=finance' in self.path:
   import xml.etree.ElementTree as ET
   from urllib.parse import unquote
   request=ET.fromstring(body);response_root=ET.Element('response');listing=ET.SubElement(ET.SubElement(response_root,'result'),'list')
   symbols=request.findall('./query/list/symbol');listing.set('count',str(len(symbols)))
   for symbol in symbols:
    quote=ET.SubElement(listing,'quote')
    values={'symbol':unquote(symbol.text),'name':'SYNTHETIC FIXTURE','sname':'FIXTURE','price':'123.45','change':'1.25','status':'1','marketcap':'1.2B','open':'122.20','high':'124.00','low':'120.00','volume':'123456','averagedailyvolume':'456789','peratio':'12.3','yearrange':'100.00 - 150.00','dividendyield':'0.5','exchange':'TEST'}
    for key,value in values.items():ET.SubElement(quote,key).text=value
   if request.find('./query').get('type')=='getchart':
    listing.clear();listing.set('count','60')
    for index in range(60):ET.SubElement(listing,'point',timestamp=str(int(time.time())-(59-index)*86400),close=str(100+index%12),volume=str(1000+index*10))
   response=ET.tostring(response_root,encoding='utf-8',xml_declaration=True);code=200
  self.send_response(code);self.send_header('Content-Type','text/xml');self.send_header('Content-Length',str(len(response)));self.end_headers();self.wfile.write(response)
 do_GET=capture;do_POST=capture;do_CONNECT=capture
 def log_message(self,*args):pass
server=None
if options.live_weather:
 routing.write_text('direct\n\n\n')
else:
 server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
 threading.Thread(target=server.serve_forever,daemon=True).start()
 routing.write_text('upstream\n127.0.0.1\n%d\n'%server.server_port)
cfg=SimpleNamespace(out=str(out),files=files,base_nand=files+'/nand-current',nor=files+'/ios3/nor_7E18.bin',overlay=str(out/'overlay'),qemu=str(root/'build-native14/qemu-build/qemu-system-arm'),usbmuxd=str(root/'build-native14/build/usbmuxd/src/usbmuxd'),usbmuxd_ok=True,usb_port=r.free_port(1520,1539),mux_port=r.free_port(27400,27419),qmp_port=r.free_port(28200,28219),wifi=True,cpu=None,mem='128M',kernel_console=True,install_timeout=420,proxy_lo=28460,proxy_hi=28479,web_proxy_config=str(routing))
p=r.Procs();d=r.Device(cfg,p,'device')
try:
 d.start();ok,detail,_=d.wait_for_home(180);assert ok,detail
 deadline=time.time()+60
 while not r.itqmp.agent_alive(d.qmp) and time.time()<deadline:time.sleep(1)
 assert r.itqmp.agent_alive(d.qmp),'guest agent unavailable'
 status,data=r.itqmp.agent(d.qmp,'put','/tmp/itproxy 755',(root/'contrib/it-proxy/itproxy').read_bytes());assert status==0,(status,data)
 status,data=r.itqmp.agent(d.qmp,'exec','chmod 755 /tmp/itproxy && /tmp/itproxy on');assert status==0,(status,data)
 control=r.AgentControl(d.qmp);ok,detail=r.unlock(cfg,control,d);assert ok,detail
 time.sleep(8)
 for app in ['com.apple.stocks' if options.stocks else 'com.apple.weather']:
  result=r.springboard(cfg,control,app);assert result.returncode==0,result
  print('LAUNCHED',app,flush=True);time.sleep(25)
  shot=d.qmp.shot(str(out/(app+'.ppm')));r.to_png(shot,str(out/(app+'.png')))
 if not options.stocks:
  d.qmp.tap(287,457)
  time.sleep(2)
  shot=d.qmp.shot(str(out/'search-settings.ppm'));r.to_png(shot,str(out/'search-settings.png'))
  d.qmp.tap(21,42)
  time.sleep(2)
  shot=d.qmp.shot(str(out/'search-entry.ppm'));r.to_png(shot,str(out/'search-entry.png'))
  # Inspected 7E18 keyboard: Cupertino for the live provider; Q for the fixture.
  keys=[(128,402),(208,297),(304,297),(80,297),(112,297),(144,297),(240,297),(224,402),(272,297)] if options.live_weather else [(16,297)]
  for x,y in keys:d.qmp.tap(x,y)
  d.qmp.tap(276,450)
  time.sleep(5)
  shot=d.qmp.shot(str(out/'search-results.ppm'));r.to_png(shot,str(out/'search-results.png'))
  d.qmp.tap(125,115)
  time.sleep(5)
  shot=d.qmp.shot(str(out/'search-added.ppm'));r.to_png(shot,str(out/'search-added.png'))
  if options.live_weather:
   d.qmp.tap(225,387)  # Celsius, then Done, on the inspected city settings screen.
   d.qmp.tap(290,42)
   time.sleep(25)
   shot=d.qmp.shot(str(out/'live-celsius.ppm'));r.to_png(shot,str(out/'live-celsius.png'))
 d.qmp.home()  # Weather persists preferences when it leaves the foreground.
 time.sleep(5)
 if options.stocks:
  status,data=r.itqmp.agent(d.qmp,'get','/var/mobile/Library/Preferences/com.apple.stocks.plist');assert status==0,(status,data)
  prefs=plistlib.loads(data)
  assert len(prefs['stocks'])==10,prefs.keys()
  assert all(stock['price']=='123.45' and stock['change']=='1.25' and stock['marketcap']=='1200000000' for stock in prefs['stocks'])
  print('PASS: native Stocks persisted ten synthetic quotes',flush=True)
 else:
  status,data=r.itqmp.agent(d.qmp,'get','/var/mobile/Library/Preferences/com.apple.weather.plist');assert status==0,(status,data)
  (out/'weather.plist').write_bytes(data)
  prefs=plistlib.loads(data)
  if options.live_weather:
   assert len(prefs['Cities'])>=3 and prefs['Celsius'],prefs.keys()
   assert any(city['Zip'].startswith('ltm:') for city in prefs['Cities'])
   # Stock Weather retains Fahrenheit data and converts it for Celsius display.
   assert all(isinstance(city.get('DataIsCelsius'),bool) and len(city.get('DayForecasts',[]))==6 and city.get('Temperature') for city in prefs['Cities'])
   print('PASS: live city search, six-day forecasts and Celsius display preference persisted',flush=True)
  else:
   assert prefs['Cities'] and all(city['Name']=='Fixture City' for city in prefs['Cities']),prefs.keys()
   assert any(request['service']==['weather'] for request in requests),requests
   assert any(city['Zip']=='ltm:37.323,-122.032:Fixture' for city in prefs['Cities']),prefs.keys()
   print('PASS: native Weather accepted synthetic forecasts and city search',flush=True)
 print('CAPTURED',len(requests),flush=True)
 assert d.powerdown(), 'shutdown not confirmed'
finally:
 if d.qmp:d.qmp.close()
 p.stop_all()
 if server:server.shutdown();server.server_close()
