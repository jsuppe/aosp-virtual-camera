import re,json
from pathlib import Path
b=Path(__file__).parent;r=b/'extracted';out={}
for run in range(831,843):
 files=list(r.rglob(f'system-{run}.txt'))
 if not files:continue
 s=files[0].read_text();parts=s.split('SNAPSHOT ')[1:]
 snap=[]
 for p in parts:
  cpu=re.search(r'^cpu\s+(.+)$',p,re.M)
  v=[int(x) for x in cpu[1].split()] if cpu else []
  state=re.search(r'\n(\d+)\n(\d+)\ncycles: (\d+) \(([^)]+) Mhz\)',p)
  snap.append({'label':p.splitlines()[0],'cpu_jiffies':v,'temperature_c':int(state[1])/1000 if state else None,'cpu_freq_khz':int(state[2]) if state else None,'gpu_clock_mhz':float(state[4]) if state else None})
 utilization=None
 if len(snap)==2:
  d=[z-a for a,z in zip(snap[0]['cpu_jiffies'],snap[1]['cpu_jiffies'])]
  utilization=(sum(d[:8])-d[3]-d[4])/sum(d[:8]) if sum(d[:8]) else None
 stats=next(r.rglob(f'trace-{run}-stats.txt')).read_text()
 drops=[int(x) for x in re.findall(r'^(?:overrun|commit overrun|dropped events): (\d+)',stats,re.M)]
 assert not any(drops),(run,drops)
 trace=next(r.rglob(f'trace-{run}.txt')).read_text()
 times=[float(x) for x in re.findall(r'\]\s+\S+\s+(\d+\.\d+):',trace)]
 freq=[int(x) for x in re.findall(r'cpu_frequency: state=(\d+)',trace)]
 out[str(run)]={'snapshots':snap,'aggregate_cpu_busy_fraction':utilization,'trace_drop_counters':drops,'trace_start':min(times),'trace_end':max(times),'cpu_frequency_events_khz_unique':sorted(set(freq))}
console=b/'console.raw'
if console.exists():
 for line in console.open(errors='replace'):
  m=re.match(r'^\[\s*(\d+\.\d+)\]\[',line)
  if not m:continue
  t=float(m[1])
  for v in out.values():
   if v['trace_start']<=t<=v['trace_end']:
    v['console_lines']=v.get('console_lines',0)+1;v['console_bytes']=v.get('console_bytes',0)+len(line.encode())
for v in out.values():
 v['console_capture_available']=console.exists()
 v.setdefault('console_lines',0 if console.exists() else None)
 v.setdefault('console_bytes',0 if console.exists() else None)
(b/'system.json').write_text(json.dumps(out,indent=2)+'\n')
for k,v in out.items():print(k,v['aggregate_cpu_busy_fraction'],[(s['temperature_c'],s['cpu_freq_khz'],s['gpu_clock_mhz']) for s in v['snapshots']],v['console_bytes'])
