import json,re
from pathlib import Path
base=Path(__file__).parent;root=base/'extracted'
results={}
for run in (701,702,703,704,705,706):
 camera=next(root.rglob(f'camera-{run}.txt')).read_text()
 # logcat is cumulative: keep the last pool of allocation records for this run.
 lines=[x for x in next(root.rglob(f'hal-{run}.txt')).read_text().splitlines() if 'PROFILE_ALLOCATION' in x]
 actual_clear=re.findall(r'PROFILE_CLEAR_OUTPUT enabled=(\d)',next(root.rglob(f'hal-{run}.txt')).read_text())
 assert actual_clear and int(actual_clear[-1])==int(run in (702,703,706)),(run,actual_clear)
 start=max(i for i,x in enumerate(lines) if ' buffer=1 ' in x)
 alloc=[]
 for x in lines[start:]:
  m=re.search(r'buffer=(\d+) usage=(0x[0-9a-f]+) modifier=(0x[0-9a-f]+) fourcc=(0x[0-9a-f]+) bytes=(\d+) status=([0-9,-]+)',x)
  assert m,x
  ident,usage,modifier,fourcc,size,status=m.groups()
  assert status=='0,0,0,0',(run,x)
  alloc.append(dict(buffer=int(ident),usage=usage,modifier=modifier,fourcc=fourcc,bytes=int(size),status=status))
 results[str(run)]={'clear_enabled':run in (702,703,706),'camera_combined_usage':re.findall(r'Combined usage: (0x[0-9a-f]+)',camera),'allocations':alloc}
text=next(root.rglob('run.log')).read_text()
for run,r in results.items():
 section=text.split('BEGIN run='+run+' ')[1].split('END run='+run)[0]
 r['gpu_clock_mhz_before_after']=[float(x) for x in re.findall(r'\(([0-9.]+) Mhz\)',section)]
 r['temperature_c']=[int(x)/1000 for x in section.splitlines() if x.isdigit() and len(x)==5]
(base/'metadata.json').write_text(json.dumps(results,indent=2)+'\n')
print(json.dumps(results,indent=2))
