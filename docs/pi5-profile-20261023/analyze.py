import csv,json,statistics,collections
from pathlib import Path
base=Path(__file__).parent
root=base/'extracted'
FAILURES=['ok','fence_dup','invalid_import','destination_timeout','cancelled','no_source','source_fence_timeout','stale_source','no_filled_output']
def rows(path):
 text=path.read_text().splitlines()
 assert 'dropped=0' in text[0],(path,text[0])
 return [{k:int(v) for k,v in r.items()} for r in csv.DictReader(x for x in text if not x.startswith('#'))]
def stats(values):
 v=sorted(values)
 if not v:return None
 def q(p):return v[min(len(v)-1,int((len(v)-1)*p))]
 return {'count':len(v),'median_ms':statistics.median(v)/1e6,'p95_ms':q(.95)/1e6,'p99_ms':q(.99)/1e6,'max_ms':v[-1]/1e6}
result={}
for run,fps,mirror,clock in [(201,60,True,True),(202,120,True,True),(203,120,False,True),(204,120,False,False),(301,120,True,True),(302,120,True,True)]:
 native=[r for f in root.rglob(f'hal-{run}-*.csv') for r in rows(f) if r['fps']==fps]
 producer=[r for f in root.rglob(f'producer-{run}-*.csv') for r in rows(f) if r['fps']==fps]
 camera=[r for f in root.rglob(f'camera-{run}-*.csv') for r in rows(f)]
 assert native and producer and camera,(run,len(native),len(producer),len(camera))
 start=min(r['start_ns'] for r in producer)+5_000_000_000;end=start+60_000_000_000
 assert max(r['start_ns'] for r in native)>=end,(run,'native coverage')
 assert max(r['start_ns'] for r in producer)>=end,(run,'producer coverage')
 assert max(r['callback_ns'] for r in camera)>=end,(run,'camera coverage')
 n=[r for r in native if start<=r['start_ns']<end]
 p=[r for r in producer if start<=r['start_ns']<end]
 c=[r for r in camera if start<=r['callback_ns']<end]
 good=[r for r in n if r['failure']==0]; successes=[r for r in c if r['status']==0]
 stages={name:stats([r[name] for r in n]) for name in n[0] if name.endswith('_ns') and name not in ('start_ns','producer_ns')}
 phase_by_failure={reason:{name:stats([r[name] for r in n if r['failure']==i]) for name in ('source_wait_ns','source_fence_wait_ns','output_ns','total_ns')} for i,reason in enumerate(FAILURES) if any(r['failure']==i for r in n)}
 proc={label:stats([r[b]-r[a] for r in p]) for label,a,b in [('vsync_to_start','vsync_ns','start_ns'),('overlay_cpu','start_ns','overlay_end_ns'),('draw_submission','overlay_end_ns','draw_end_ns'),('source_swap','draw_end_ns','swap_end_ns'),('mirror_draw_swap','swap_end_ns','mirror_end_ns'),('render_iteration','start_ns','mirror_end_ns')]}
 arrivals=[r['callback_ns']-r['producer_ns'] for r in successes if r['producer_ns']>0]
 seqs=[r['source_sequence'] for r in good];regressions=sum(b<=a for a,b in zip(seqs,seqs[1:]))
 result[str(run)]={'fps_target':fps,'mirror':mirror,'clock':clock,'window_start_ns':start,'window_seconds':60,'producer_submissions_per_second':len(p)/60,'hal_attempts_per_second':len(n)/60,'hal_successes_per_second':len(good)/60,'camera2_success_results_per_second':len(successes)/60,'camera2_failed_results_per_second':sum(r['status']!=0 for r in c)/60,'failure_counts':dict(collections.Counter(FAILURES[r['failure']] for r in n)),'hal_stage_cpu_elapsed':stages,'hal_stages_by_outcome':phase_by_failure,'producer_cpu_elapsed':proc,'producer_timestamp_to_result_callback':stats(arrivals),'negative_callback_latencies':sum(v<0 for v in arrivals),'missing_producer_timestamps':sum(r['producer_ns']==0 for r in successes),'successful_source_sequence_regressions':regressions,'successful_source_sequence_gaps':sum(max(0,b-a-1) for a,b in zip(seqs,seqs[1:]))}
summary={'schema':1,'scope':'Single Pi5 Android17 1920x1080RGBA Camera2 TextureView;60s per case after5s warmup. Raw sequence identities are HAL submissions, not independently decoded pixel markers. CPU elapsed GPU API/fence waits are not GPU execution timers. Callback latency is not physical display latency.','runs':result}
(base/'analysis.json').write_text(json.dumps(summary,indent=2)+'\n')
for k,r in result.items():print(k,r['producer_submissions_per_second'],r['camera2_success_results_per_second'],r['failure_counts'])
