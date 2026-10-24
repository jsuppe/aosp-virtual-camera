import csv,json,statistics
from pathlib import Path
base=Path(__file__).parent
root=base/'extracted'
def stats(v):
 v=sorted(v)
 return {'n':len(v),'median_ms':statistics.median(v)/1e6,'p95_ms':v[int((len(v)-1)*.95)]/1e6,'p99_ms':v[int((len(v)-1)*.99)]/1e6,'max_ms':v[-1]/1e6} if v else None
out={}
for run in range(811,817):
 p=next(root.rglob(f'{run}-images.csv'))
 rows=[{k:int(v) for k,v in r.items()} for r in csv.DictReader(p.open())]
 status=json.loads(next(root.rglob(f'{run}-status.json')).read_text())
 c=status['cameras']['100']
 first=rows[0]['receipt_ns']+5_000_000_000;end=first+30_000_000_000
 assert rows[-1]['receipt_ns']>=end,(run,'short window')
 r=[x for x in rows if first<=x['receipt_ns']<end]
 ids=[(x['producer'],x['run'],x['generation'],x['sequence']) for x in r]
 results=[{k:int(v) for k,v in x.items()} for x in csv.DictReader(next(root.rglob(f'{run}-results.csv')).open())]
 byts={x['sensor_timestamp_ns']:x for x in results}
 intervals=[b['receipt_ns']-a['receipt_ns'] for a,b in zip(r,r[1:])]
 out[str(run)]={'status':status,'window_seconds':30,'decoded_unique_frames_per_second':len(set(ids))/30,'duplicates':len(ids)-len(set(ids)),'sequence_regressions':sum(a['generation']==b['generation'] and b['sequence']<=a['sequence'] for a,b in zip(r,r[1:])),'receipt_interval':stats(intervals),'image_timestamp_to_receipt':stats([x['receipt_ns']-x['image_timestamp_ns'] for x in r]),'image_result_timestamp_matches':sum(x['image_timestamp_ns'] in byts for x in r),'images_in_window':len(r)}
lifecycle={p.stem:json.loads(p.read_text()) for p in root.rglob('lifecycle-*.json')}
negative=json.loads(next(root.rglob('wrong-identity-negative.json')).read_text())
a=lifecycle['lifecycle-stall-a']['cameras']['100'];b=lifecycle['lifecycle-stall-b']['cameras']['100']
checks={'stall_no_new_images':a['images']==b['images'],'stall_failed_attempts_continue':b['capture_failures']>a['capture_failures'],'resumed_images':lifecycle['lifecycle-resumed']['cameras']['100']['images']>b['images'],'positive_failure_strings_empty':all(not x['failure'] for x in lifecycle.values()),'negative_rejected':'wrong producer/run' in negative['failure']}
order=['lifecycle-live','lifecycle-resumed','lifecycle-reopened','lifecycle-1080','lifecycle-120','lifecycle-60','lifecycle-producer-restarted']
checks['images_progress_after_each_transition']=all(lifecycle[y]['cameras']['100']['images']>lifecycle[x]['cameras']['100']['images'] for x,y in zip(order,order[1:]))
checks['reopen_new_generation']=lifecycle['lifecycle-reopened']['cameras']['100']['generation']>lifecycle['lifecycle-resumed']['cameras']['100']['generation']
checks['producer_death_disconnected']=lifecycle['lifecycle-producer-dead']['cameras']['100']['disconnects']>=1
checks['producer_restart_expected_identity']=lifecycle['lifecycle-producer-restarted']['cameras']['100']['expected_run']==819
checks['requested_120_metadata']=lifecycle['lifecycle-120']['cameras']['100']['frame_duration_ns']==8333334
report={'cases':out,'lifecycle':lifecycle,'negative':negative,'checks':checks,'scope':'RGBA marker cells and monotonic identity, not full-image golden comparison; CPU readback and marker workload differs from Surface demo'}
(base/'qualification.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({'rates':{k:v['decoded_unique_frames_per_second'] for k,v in out.items()},'checks':checks},indent=2))
