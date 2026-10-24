import csv,json,statistics,collections
from pathlib import Path
base=Path(__file__).parent;root=base/'extracted'
def rows(p):
 text=p.read_text().splitlines()
 assert 'dropped=0' in text[0],(p,text[0])
 return [{k:int(v) for k,v in r.items()} for r in csv.DictReader(x for x in text if not x.startswith('#'))]
def stats(v):
 v=sorted(v)
 return {'n':len(v),'median_ms':statistics.median(v)/1e6,'p95_ms':v[int((len(v)-1)*.95)]/1e6,'p99_ms':v[int((len(v)-1)*.99)]/1e6,'max_ms':v[-1]/1e6} if v else None
results={}
for run in (701,702,703,704,705,706):
 p=[r for f in root.rglob(f'producer-{run}-*.csv') for r in rows(f) if r['fps']==120]
 c=[r for f in root.rglob(f'camera-{run}-*.csv') for r in rows(f)]
 assert p and c
 start=min(r['start_ns'] for r in p)+5_000_000_000;end=start+10_000_000_000
 assert max(r['start_ns'] for r in p)>=end and max(r['callback_ns'] for r in c)>=end
 p=[r for r in p if start<=r['start_ns']<end];c=[r for r in c if start<=r['callback_ns']<end]
 good=[r for r in c if r['status']==0]
 lat=[r['callback_ns']-r['producer_ns'] for r in good if r['producer_ns']>0]
 results[str(run)]={'preview':'SurfaceView' if run in (701,702,703,704) else 'TextureView','mirror_and_clock':False,'fps_target':120,'window_seconds':10,'window_start_ns':start,'producer_submissions_per_second':len(p)/10,'camera2_success_results_per_second':len(good)/10,'camera2_failed_results_per_second':sum(r['status']!=0 for r in c)/10,'producer_timestamp_to_result_callback':stats(lat),'missing_timestamps':sum(r['producer_ns']==0 for r in good),'negative_latencies':sum(x<0 for x in lat),'producer_cpu_elapsed':{label:stats([r[b]-r[a] for r in p]) for label,a,b in [('overlay','start_ns','overlay_end_ns'),('draw','overlay_end_ns','draw_end_ns'),('swap','draw_end_ns','swap_end_ns'),('mirror','swap_end_ns','mirror_end_ns')]}}
(base/'analysis.json').write_text(json.dumps(results,indent=2)+'\n')
for k,r in results.items():print(k,r['producer_submissions_per_second'],r['camera2_success_results_per_second'],r['producer_timestamp_to_result_callback'])
