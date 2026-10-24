import csv,io,json,subprocess,statistics,os
from pathlib import Path
b=Path(__file__).parent;r=b/'extracted'
def query(p,sql):
 v=subprocess.run([os.environ.get('TRACE_PROCESSOR','trace_processor'),'query',str(p),sql],text=True,capture_output=True,check=True)
 return list(csv.DictReader(io.StringIO(v.stdout)))
def stat(v):
 v=sorted(v)
 return {'n':len(v),'median_ms':statistics.median(v)/1e6,'p95_ms':v[int((len(v)-1)*.95)]/1e6,'p99_ms':v[int((len(v)-1)*.99)]/1e6,'max_ms':v[-1]/1e6} if v else None
out={}
for run in (838,881,882,883):
 files=list(r.rglob(f'present-{run}.pftrace'))
 if not files:continue
 p=files[0]
 frames=query(p,'SELECT ts,dur,jank_type,present_type,gpu_composition FROM actual_frame_timeline_slice WHERE layer_name IS NULL ORDER BY ts')
 good=[x for x in frames if int(x['dur'])>=0 and x['present_type'] in ('On-time Present','Early Present','Late Present')]
 ends=[int(x['ts'])+int(x['dur']) for x in good]
 out[str(run)]={'display_frames':len(frames),'presented_records':len(good),'out_of_order_completion_count':sum(z<a for a,z in zip(ends,ends[1:])),'software_display_frame_duration':stat([int(x['dur']) for x in good]),'display_frame_completion_interval':stat([z-a for a,z in zip(sorted(ends),sorted(ends)[1:])]),'jank_counts':query(p,'SELECT jank_type,present_type,gpu_composition,count(*) AS n FROM actual_frame_timeline_slice WHERE layer_name IS NULL GROUP BY 1,2,3'),'layer_counts':query(p,'SELECT layer_name,count(*) AS n FROM actual_frame_timeline_slice GROUP BY layer_name'),'scheduler_rows':query(p,'SELECT count(*) AS n FROM sched'),'cpu_by_thread':query(p,"SELECT thread.name,sum(sched.dur)/1e9 AS cpu_s FROM sched JOIN thread USING(utid) WHERE sched.dur>=0 GROUP BY thread.name ORDER BY cpu_s DESC LIMIT 15"),'event_counts':query(p,'SELECT name,count(*) AS n FROM ftrace_event GROUP BY name ORDER BY n DESC'),'nonzero_stats':query(p,'SELECT name,value,severity FROM stats WHERE value != 0'),'scope':'Display frames, not uniquely decoded camera buffers or photons. Initial832/835 captures excluded due probe crash.'}
for run,v in out.items():
 p=next(r.rglob(f'present-{run}.pftrace'))
 events=query(p,"SELECT ts,EXTRACT_ARG(arg_set_id,'seq') AS seq,EXTRACT_ARG(arg_set_id,'time') AS vblank_time FROM ftrace_event WHERE name='drm_vblank_event' ORDER BY ts")
 normalized=[];gaps=[]
 for a,z in zip(events,events[1:]):
  step=int(z['seq'])-int(a['seq'])
  if step>0:
   normalized.append((int(z['vblank_time'])-int(a['vblank_time']))/step)
   if step>1:gaps.append(step)
 v['kms_vblank_period_normalized_by_sequence']=stat(normalized)
 v['kms_trace_event_sequence_gaps']={'count':len(gaps),'largest_sequence_step':max(gaps,default=1),'interpretation':'Missing logged events are not proof of stopped physical scanout or a missed camera frame.'}
(b/'presentation.json').write_text(json.dumps(out,indent=2)+'\n')
for k,v in out.items():print(k,v['display_frames'],v['scheduler_rows'],v['jank_counts'])
