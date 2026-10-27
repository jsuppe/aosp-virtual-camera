import csv,json,statistics,collections,sys
from pathlib import Path
base=Path(sys.argv[1]);root=base/'extracted'
def rows(p):
 lines=p.read_text().splitlines()
 assert 'dropped=0' in lines[0],(p,lines[0])
 return [{k:int(v) for k,v in r.items()} for r in csv.DictReader(x for x in lines if not x.startswith('#'))]
def stats(v):
 v=sorted(v)
 return {'n':len(v),'median_ms':statistics.median(v)/1e6,'p95_ms':v[int((len(v)-1)*.95)]/1e6,'p99_ms':v[int((len(v)-1)*.99)]/1e6,'max_ms':v[-1]/1e6} if v else None
out={}
for run in range(1001,1100):
 p=[r for f in root.rglob(f'producer-{run}-*.csv') for r in rows(f)]
 c=[r for f in root.rglob(f'camera-{run}-*.csv') for r in rows(f)]
 h=[r for f in root.rglob(f'hal-{run}-*.csv') for r in rows(f)]
 if not p or not c:continue
 start=min(r['start_ns'] for r in p)+15_000_000_000;end=start+60_000_000_000
 assert max(r['start_ns'] for r in p)>=end and max(r['callback_ns'] for r in c)>=end,(run,'short')
 p=[r for r in p if start<=r['start_ns']<end];c=[r for r in c if start<=r['callback_ns']<end];h=[r for r in h if start<=r['start_ns']<end]
 good=[r for r in c if r['status']==0];hg=[r for r in h if r['failure']==0]
 latency=[r['callback_ns']-r['producer_ns'] for r in good if r['producer_ns']>0]
 signal=[r['fence_observed_ns']-r['fence_signal_ns'] for r in h if r['fence_status']==1 and r['fence_signal_ns']>0]
 times=sorted(r['callback_ns'] for r in good)
 result={'window_seconds':60,'window_start_ns':start,'producer_submissions_per_second':len(p)/60,'camera2_success_results_per_second':len(good)/60,'camera2_failed_results_per_second':(len(c)-len(good))/60,'result_interval':stats([b-a for a,b in zip(times,times[1:])]),'producer_timestamp_to_result_callback':stats(latency),'missing_timestamps':sum(r['producer_ns']==0 for r in good),'negative_latencies':sum(t<0 for t in latency),'producer_cpu_elapsed':{label:stats([r[b]-r[a] for r in p]) for label,a,b in [('overlay','start_ns','overlay_end_ns'),('draw','overlay_end_ns','draw_end_ns'),('swap','draw_end_ns','swap_end_ns'),('mirror','swap_end_ns','mirror_end_ns')]},'hal_failures':dict(collections.Counter(r['failure'] for r in h)),'hal_success_cpu_phases':{key:stats([r[key] for r in hg]) for key in ['import_ns','destination_wait_ns','pace_ns','source_wait_ns','source_fence_wait_ns','output_ns','callback_ns','cleanup_ns','total_ns','gpu_context_ns','gpu_source_import_ns','gpu_destination_import_ns','gpu_draw_submit_ns','gpu_fence_submit_ns','gpu_cleanup_ns']},'source_fence_signal_to_observed':stats(signal),'hal_success_unique_source_sequences':len(set(r['source_sequence'] for r in hg)),'hal_success_rows':len(hg),'source_acquisition_age':stats([r['source_acquired_ns']-r['producer_ns'] for r in hg if r['producer_ns']>0])}
 result['per_10s_success_rates']=[sum(start+i*10**10<=r['callback_ns']<start+(i+1)*10**10 for r in good)/10 for i in range(6)]
 out[str(run)]=result
(base/'performance.json').write_text(json.dumps(out,indent=2)+'\n')
for k,v in out.items():print(k,round(v['producer_submissions_per_second'],2),round(v['camera2_success_results_per_second'],2),v['producer_timestamp_to_result_callback'])
