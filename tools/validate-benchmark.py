#!/usr/bin/env python3
"""Validate raw observations against summaries; never assert a universal deadline."""
import csv,math,pathlib,statistics,sys
root=pathlib.Path(sys.argv[1]); groups={}
key=lambda r:tuple(r[k] for k in ('scenario','branches','pdc','outputs','frames'))
for r in csv.DictReader((root/'raw.csv').open()): groups.setdefault(key(r),[]).append(float(r['duration_us']))
rows=list(csv.DictReader((root/'summary.csv').open()))
assert len(rows)==120 and len(groups)==120
for r in rows:
 values=sorted(groups[key(r)]); assert len(values)==100 and all(math.isfinite(v) and v>=0 for v in values)
 expected={'mean_us':statistics.mean(values),'median_us':values[49],'p95_us':values[94],'p99_us':values[98],'max_us':values[-1]}
 for k,v in expected.items(): assert abs(float(r[k])-v)<0.000002,(key(r),k)
 assert int(r['pool_bytes'])==int(r['peak_slots'])*int(r['frames'])*4
 assert int(r['history_bytes'])==int(r['pdc'])*2*int(r['branches'])*64*4
 assert int(r['max_latency'])==int(r['pdc'])*64
 assert all(int(r[k])==0 for k in ('allocations','frees','mutexes','waits','logs','capacity_diagnostics'))
 assert int(r['refused_candidates'])==1
 assert int(r['overruns'])==sum(v>1e6*int(r['frames'])/48000 for v in values)
 assert int(r['publications'])==(12 if r['scenario']=='publish' else 0)
print('Validated 120 scenarios / 12000 observations; zero forbidden RT operations.')
