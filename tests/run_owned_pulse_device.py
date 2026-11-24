#!/usr/bin/env python3
"""Exercise native device transfer on an owned clocked endpoint, with no hardware access."""
import argparse,array,json,os,shutil,subprocess,tempfile,time
from pathlib import Path
parser=argparse.ArgumentParser();parser.add_argument('--probe',required=True);parser.add_argument('--output',required=True);parser.add_argument('--runtime-prefix');parser.add_argument('--module-directory');args=parser.parse_args()
out=Path(args.output).resolve();out.mkdir(parents=True,exist_ok=True)
prefix=Path(args.runtime_prefix).resolve() if args.runtime_prefix else None
binary=lambda name:str(prefix/'usr/bin'/name) if prefix else shutil.which(name)
server=None;monitor=None
with tempfile.TemporaryDirectory(prefix='cell-audio-')as temporary:
 root=Path(temporary);env=os.environ.copy();env.update(PULSE_RUNTIME_PATH=str(root),PULSE_STATE_PATH=str(root/'state'),PULSE_CONFIG_PATH=str(root),PULSE_COOKIE=str(root/'cookie'),XDG_RUNTIME_DIR=str(root),PULSE_SERVER='unix:'+str(root/'socket'))
 (root/'state').mkdir();(root/'cookie').write_bytes(os.urandom(256));(root/'cookie').chmod(0o600)
 if prefix:
  libraries=[prefix/'usr/lib/x86_64-linux-gnu',prefix/'usr/lib/x86_64-linux-gnu/pulseaudio'];libraries+=([Path(args.module_directory)] if args.module_directory else []);env['LD_LIBRARY_PATH']=':'.join(map(str,libraries))+':'+env.get('LD_LIBRARY_PATH','')
 (root/'test.pa').write_text('.fail\nload-module module-native-protocol-unix socket='+str(root/'socket')+' auth-anonymous=1 auth-cookie='+str(root/'cookie')+'\nload-module module-null-sink sink_name=cell_native_test format=float32le rate=48000 channels=2 channel_map=front-left,front-right\nset-default-sink cell_native_test\nset-default-source cell_native_test.monitor\n')
 command=[binary('pulseaudio'),'--daemonize=no','--exit-idle-time=-1','--use-pid-file=no','--disable-shm=yes','-n','--file='+str(root/'test.pa')]
 if args.module_directory:command+=['--dl-search-path='+args.module_directory]
 try:
  with (out/'server.log').open('w')as log:server=subprocess.Popen(command,env=env,stdout=log,stderr=subprocess.STDOUT)
  deadline=time.monotonic()+10
  while not(root/'socket').exists():
   if server.poll() is not None:raise RuntimeError('Owned audio server exited; see server.log')
   if time.monotonic()>deadline:raise RuntimeError('Owned audio endpoint did not become available')
   time.sleep(.01)
  subprocess.run([binary('pactl'),'info'],env=env,check=True,stdout=subprocess.DEVNULL)
  with (out/'monitor.f32').open('wb')as capture,(out/'monitor.log').open('w')as log:
   monitor=subprocess.Popen([binary('parec'),'--raw','--format=float32le','--rate=48000','--channels=2','--device=cell_native_test.monitor','--latency-msec=5'],env=env,stdout=capture,stderr=log)
   time.sleep(.15)
   result=subprocess.run([str(Path(args.probe).resolve()),str(out/'native-output.f32')],env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=30)
   (out/'probe.log').write_text(result.stdout);result.check_returncode();time.sleep(.05)
   monitor.terminate();monitor.wait(timeout=3);monitor=None
  native=(out/'native-output.f32').read_bytes();recorded=(out/'monitor.f32').read_bytes();values=array.array('f');values.frombytes(recorded)
  if not values or len(values)%2:raise RuntimeError('No complete stereo monitor recording')
  peak=max(map(abs,values));polarity=max(abs(a+b)for a,b in zip(values[0::2],values[1::2]))
  matches=[frame for frame in (2048,4096,8192,16384,24576,32768)if len(native)>=(frame+128)*8 and recorded.find(native[frame*8:(frame+128)*8])>=0]
  if peak<.1 or polarity>1e-8 or not matches:raise RuntimeError('Native output did not reach the monitor with exact PCM and channel polarity')
  report={'passed':True,'backend':'PulseAudio','hardware':False,'sample_rate':48000,'requested_quantum':128,'recorded_frames':len(values)//2,'peak':peak,'stereo_polarity_error':polarity,'bit_exact_128_frame_windows_at_source_frames':matches,'scope':'Owned virtual endpoint, actual native device API and independent monitor capture. No physical-speaker latency or hardware deadline claim.'}
  (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(result.stdout);print(json.dumps(report))
 finally:
  for process in (monitor,server):
   if process is not None and process.poll() is None:
    process.terminate()
    try:process.wait(timeout=3)
    except subprocess.TimeoutExpired:process.kill();process.wait()
