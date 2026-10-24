import re,json,statistics,collections
from pathlib import Path
base=Path(__file__).parent
root=base/'extracted'
def stats(v):
 v=sorted(v)
 if not v:return None
 return {'n':len(v),'median_ms':statistics.median(v)*1000,'p95_ms':v[int((len(v)-1)*.95)]*1000,'p99_ms':v[int((len(v)-1)*.99)]*1000,'max_ms':v[-1]*1000}
def union(intervals):
 end=-1;total=0
 for a,b in sorted(intervals):
  total+=max(0,b-max(a,end));end=max(end,b)
 return total
results={}
for run in (701,702,703,704,705,706):
 files=list(root.rglob(f'trace-{run}.txt'))
 if not files:continue
 p=files[0]
 hal_pid='5209'
 event=re.compile(r'\[(\d+)\]\s+\S+\s+(\d+\.\d+):\s+(\w+):\s+(.*)')
 starts={}; intervals=collections.defaultdict(list);waiting={};wake=collections.defaultdict(list)
 cpu={};cpu_time=collections.Counter();counts=collections.Counter();unmatched=collections.Counter();first=None;last=None
 def group(name):
  if 'VCamRenderGL' in name:return 'producer_render'
  if name.startswith('binder:'+hal_pid):return 'hal_binder'
  if 'vcamproducer' in name:return 'app_other'
  return None
 for line in p.open():
  m=event.search(line)
  if not m:continue
  c,t,e,d=m.groups();t=float(t);counts[e]+=1
  if first is None:first=t
  last=t
  if e=='v3d_submit_cl':
   q=re.search(r'(RCL|BCL), seqno=(\d+)',d)
   if q:starts[(q[1],q[2])]=t
  elif e in ('v3d_rcl_irq','v3d_bcl_irq'):
   q=re.search(r'seqno=(\d+)',d)
   if q:
    kind='RCL' if e=='v3d_rcl_irq' else 'BCL';key=(kind,q[1])
    a=starts.pop(key,None)
    if a is not None:intervals[kind].append((a,t))
    else:unmatched[kind]+=1
  elif e=='sched_wakeup':
   q=re.search(r'comm=(.*?) pid=(\d+)',d)
   if q and group(q[1]):waiting.setdefault(q[2],(t,group(q[1])))
  elif e=='sched_switch':
   q=re.search(r'prev_comm=(.*?) prev_pid=(\d+).*?prev_state=(\S+).*?next_comm=(.*?) next_pid=(\d+)',d)
   if not q:continue
   prev,pid,state,nxt,nid=q.groups()
   if c in cpu:
    a,g=cpu[c]
    if g:cpu_time[g]+=t-a
   g=group(nxt);cpu[c]=(t,g)
   if nid in waiting:
    a,wg=waiting.pop(nid);wake[wg].append(t-a)
   pg=group(prev)
   if pg and state.startswith('R'):waiting.setdefault(pid,(t,pg))
 duration=last-first
 results[str(run)]={'trace_seconds':duration,'gpu_job_service':{k:stats([b-a for a,b in v]) for k,v in intervals.items()},'gpu_engine_busy_fraction':{k:union(v)/duration for k,v in intervals.items()},'unmatched_initial_irqs':dict(unmatched),'unfinished_boundary_jobs':len(starts),'runnable_to_cpu':{k:stats(v) for k,v in wake.items()},'scheduled_cpu_seconds':dict(cpu_time),'event_counts':dict(counts)}
(base/'trace-analysis.json').write_text(json.dumps(results,indent=2)+'\n')
for k,v in results.items():print(k,json.dumps(v))
