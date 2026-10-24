import re,json
from pathlib import Path
b=Path(__file__).parent;r=b/'extracted';out={}
for run in list(range(831,843))+[881,882,883]:
 files=list(r.rglob(f'sf-{run}.txt'))
 if not files:continue
 s=files[0].read_text();lines=s.splitlines();layers=[]
 for a,z in zip(lines,lines[1:]):
  if 'SplitDemoActivity' in a and re.search(r'\|\s+(DEVICE|CLIENT)\s+\|',z):
   layers.append({'role':'camera_surface' if 'SurfaceView' in a else 'app_window','composition':re.search(r'\|\s+(DEVICE|CLIENT)\s+\|',z)[1]})
 cfiles=list(r.rglob(f'camera-{run}.txt'));c=cfiles[0].read_text() if cfiles else ''
 dequeue=re.search(r'DequeueBuffer latency histogram:.*\n(.*)\n(.*)',c)
 out[str(run)]={'active_modes':sorted(set(re.findall(r'activeMode=\{([^}]+)\}',s))),'comparison_layers':layers,'stream_usage':re.findall(r'Combined usage: (.+)',c),'buffer_counts':re.findall(r'Total buffers: (.+)',c),'dequeue_histogram':dequeue[0] if dequeue else None,'scope':'One post-window dump; recording has ended before the839dump, so it does not prove recording-time composition.'}
(b/'composition.json').write_text(json.dumps(out,indent=2)+'\n')
for k,v in out.items():print(k,v['comparison_layers'],v['stream_usage'],v['buffer_counts'])
