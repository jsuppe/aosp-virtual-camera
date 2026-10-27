import sys,re,json,collections,statistics
from pathlib import Path
base=Path(sys.argv[1]);root=base/'extracted'
event=re.compile(r'^\s*(.+)-(\d+)\s+\[\d+\]\s+\S+\s+(\d+\.\d+):\s+(\w+):\s+(.*)')
def stats(v):
 v=sorted(v)
 return {'n':len(v),'median_ms':statistics.median(v)*1000,'p95_ms':v[int((len(v)-1)*.95)]*1000,'p99_ms':v[int((len(v)-1)*.99)]*1000,'max_ms':v[-1]*1000} if v else None
results={}
for run in range(1001,1100):
 files=list(root.rglob(f'trace-{run}.txt'))
 if not files:continue
 p=files[0]
 halpid=next(root.rglob(f'hal-{run}-*.csv')).name.split('-')[2]
 queued=collections.defaultdict(collections.deque);active={};groups=collections.defaultdict(list);threads=collections.Counter()
 unmatched=0;first=last=None
 job_origins={};dispatch={};crosschecked=0;mismatch=0
 for line in p.open():
  m=event.match(line)
  if not m:continue
  name,pid,t,e,d=m.groups();t=float(t)
  if first is None:first=t
  last=t
  if e=='drm_sched_job_queue':
   q=re.search(r'fence=(\d+:\d+)',d)
   if q:job_origins[q[1]]=(name,pid)
  elif e=='drm_sched_job_run':
   q=re.search(r'fence=(\d+:\d+), ring=(\w+)',d)
   if q:dispatch[pid]=(job_origins.get(q[1]),q[2])
  elif e=='v3d_submit_cl_ioctl':
   q=re.search(r'RCL (0x[0-9a-f]+\.\.0x[0-9a-f]+)',d)
   if q:queued[q[1]].append((t,name,pid))
  elif e=='v3d_submit_cl':
   q=re.search(r'RCL, seqno=(\d+), (0x[0-9a-f]+\.\.0x[0-9a-f]+)',d)
   if q:
    seq,addr=q.groups();origin=queued[addr].popleft() if queued[addr] else None
    other,ring=dispatch.get(pid,(None,None))
    if origin is not None and other is not None and ring=='v3d_render':
     crosschecked+=1
     if (origin[1],origin[2])!=other:mismatch+=1
    active[seq]=(t,origin)
  elif e=='v3d_rcl_irq':
   q=re.search(r'seqno=(\d+)',d)
   if q and q[1] in active:
    start,origin=active.pop(q[1])
    if origin is None:unmatched+=1;continue
    submitted,name,pid=origin;threads[name+'-'+pid]+=1
    group=('producer' if name=='VCamRenderGL' else 'hal' if pid==halpid or name.startswith('binder:'+halpid) else 'android_RenderThread' if name=='RenderThread' else name)
    groups[group].append((start-submitted,t-start,t-submitted))
 results[str(run)]={'independent_fence_join_crosschecks':crosschecked,'independent_fence_join_mismatches':mismatch,'matched_jobs':sum(len(x) for x in groups.values()),'unmatched_boundary_jobs':unmatched,'window_seconds':last-first,'groups':{k:{'queue_to_hardware':stats([x[0] for x in v]),'hardware_to_irq':stats([x[1] for x in v]),'submit_to_irq':stats([x[2] for x in v]),'render_engine_time_share':sum(x[1] for x in v)/(last-first)} for k,v in groups.items()},'origin_threads':dict(threads)}
(base/'gpu-attribution.json').write_text(json.dumps(results,indent=2)+'\n')
for k,r in results.items():
 print(k)
 for g,v in r['groups'].items():print(g,v)
