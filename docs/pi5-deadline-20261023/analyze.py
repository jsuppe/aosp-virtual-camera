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
for run,fps,mirror,clock in [(401,120,False,False),(402,120,False,False),(403,120,False,False),(404,120,False,False),(405,120,False,False)]:
 native=[r for f in root.rglob(f'hal-{run}-*.csv') for r in rows(f) if r['fps']==fps]
 producer=[r for f in root.rglob(f'producer-{run}-*.csv') for r in rows(f) if r['fps']==fps]
 camera=[r for f in root.rglob(f'camera-{run}-*.csv') for r in rows(f)]
 assert all(r['width']==1920 and r['height']==1080 for r in native)
 assert native and producer and camera,(run,len(native),len(producer),len(camera))
 start=min(r['start_ns'] for r in producer)+5_000_000_000;end=start+60_000_000_000
 assert max(r['start_ns'] for r in native)>=end,(run,'native coverage')
 assert max(r['start_ns'] for r in producer)>=end,(run,'producer coverage')
 assert max(r['callback_ns'] for r in camera)>=end,(run,'camera coverage')
 n=[r for r in native if start<=r['start_ns']<end]
 p=[r for r in producer if start<=r['start_ns']<end]
 c=[r for r in camera if start<=r['callback_ns']<end]
 good=[r for r in n if r['failure']==0]; successes=[r for r in c if r['status']==0]
 stages={name:stats([r[name] for r in n]) for name in n[0] if name.endswith('_ns') and name in ('import_ns','destination_wait_ns','pace_ns','source_wait_ns','source_fence_wait_ns','output_ns','callback_ns','cleanup_ns','total_ns')}
 phase_by_failure={reason:{name:stats([r[name] for r in n if r['failure']==i]) for name in ('source_wait_ns','source_fence_wait_ns','output_ns','total_ns')} for i,reason in enumerate(FAILURES) if any(r['failure']==i for r in n)}
 proc={label:stats([r[b]-r[a] for r in p]) for label,a,b in [('vsync_to_start','vsync_ns','start_ns'),('overlay_cpu','start_ns','overlay_end_ns'),('draw_submission','overlay_end_ns','draw_end_ns'),('source_swap','draw_end_ns','swap_end_ns'),('mirror_draw_swap','swap_end_ns','mirror_end_ns'),('render_iteration','start_ns','mirror_end_ns')]}
 arrivals=[r['callback_ns']-r['producer_ns'] for r in successes if r['producer_ns']>0]
 readiness=[r for r in n if r['source_acquired_ns']>0 and r['deadline_ns']>0]
 signal=[r for r in readiness if r['fence_status']==1 and r['fence_signal_ns']>0]
 waits=[r for r in readiness if r['failure']==6]
 extras={'minimal_scene':run in (402,403,405),'deadline_frames':2 if run in (403,404) else 1,
  'signal_after_acquisition_to_observation':stats([r['fence_observed_ns']-r['fence_signal_ns'] for r in signal if r['fence_signal_ns']>r['source_acquired_ns']]),
  'already_signaled_at_acquisition':sum(r['fence_signal_ns']<=r['source_acquired_ns'] for r in signal),
  'signal_to_observation':stats([r['fence_observed_ns']-r['fence_signal_ns'] for r in signal]),
  'producer_to_fence_signal':stats([r['fence_signal_ns']-r['producer_ns'] for r in signal]),
  'budget_remaining_at_acquisition':stats([r['deadline_ns']-r['source_acquired_ns'] for r in readiness]),
  'timeout_fence_statuses':dict(collections.Counter(str(r['fence_status']) for r in waits)),
  'timeout_signaled_by_query_before_deadline':sum(r['fence_status']==1 and 0<r['fence_signal_ns']<=r['deadline_ns'] for r in waits),
  'successful_signal_after_deadline':sum(r['failure']==0 and r['fence_signal_ns']>r['deadline_ns'] for r in signal),
  'fence_snapshot_count':len(readiness),'valid_signaled_fence_timestamps':len(signal),
  'fence_snapshot_statuses':dict(collections.Counter(str(r['fence_status']) for r in readiness)),
  'signaled_without_timestamp':sum(r['fence_status']==1 and r['fence_signal_ns']==0 for r in readiness),
  'fence_signal_before_producer_timestamp':sum(r['fence_signal_ns']<r['producer_ns'] for r in signal)}
 seqs=[r['source_sequence'] for r in good];regressions=sum(b<=a for a,b in zip(seqs,seqs[1:]))
 result[str(run)]={**extras,'fps_target':fps,'mirror':mirror,'clock':clock,'window_start_ns':start,'window_seconds':60,'producer_submissions_per_second':len(p)/60,'hal_attempts_per_second':len(n)/60,'hal_successes_per_second':len(good)/60,'camera2_success_results_per_second':len(successes)/60,'camera2_failed_results_per_second':sum(r['status']!=0 for r in c)/60,'failure_counts':dict(collections.Counter(FAILURES[r['failure']] for r in n)),'hal_stage_cpu_elapsed':stages,'hal_stages_by_outcome':phase_by_failure,'producer_cpu_elapsed':proc,'producer_timestamp_to_result_callback':stats(arrivals),'negative_callback_latencies':sum(v<0 for v in arrivals),'missing_producer_timestamps':sum(r['producer_ns']==0 for r in successes),'successful_source_sequence_regressions':regressions,'successful_source_sequence_gaps':sum(max(0,b-a-1) for a,b in zip(seqs,seqs[1:]))}
summary={'schema':1,'scope':'Single Pi5 Android17 1920x1080RGBA Camera2 TextureView;60s per case after5s warmup. Raw sequence identities are HAL submissions, not independently decoded pixel markers. CPU elapsed GPU API/fence waits are not GPU execution timers. Callback latency is not physical display latency.','runs':result}
(base/'analysis.json').write_text(json.dumps(summary,indent=2)+'\n')
for k,r in result.items():print(k,r['producer_submissions_per_second'],r['camera2_success_results_per_second'],r['failure_counts'])
