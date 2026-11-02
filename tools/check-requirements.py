#!/usr/bin/env python3
"""Verify traceability against real CTest discovery and successful executions."""
import argparse,json,pathlib,re,subprocess
p=argparse.ArgumentParser();p.add_argument('--build',default='build/debug');p.add_argument('--ctest',default='ctest');a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
m=root/'MKA_Audio_DSP_V1_Documentation_2026-10-31/mka-dsp-v1-documentation/04-validation/MATRICE_EXIGENCES.md'
s=m.read_text(); ids=re.findall(r'^\| ((?:G|B|IO|SW|P)-\d\d) \|',s,re.M)
expected={f'{prefix}-{i:02}' for prefix,n in [('G',11),('B',10),('IO',10),('SW',8),('P',12)] for i in range(1,n+1)}
assert set(ids)==expected and len(ids)==len(expected)
log=(pathlib.Path(a.build)/'Testing/Temporary/LastTest.log').read_text()
# CTest discovery itself replaces LastTest.log; retain the execution evidence.
try:
 names={x['name'] for x in json.loads(subprocess.check_output([a.ctest,'--test-dir',a.build,'--show-only=json-v1'],text=True))['tests']}
finally:
 (pathlib.Path(a.build)/'Testing/Temporary/LastTest.log').write_text(log)
passed={name for name,body in re.findall(r'\d+/\d+ Test: ([^\n]+)\n(.*?)(?=\n\d+/\d+ Test:|\Z)',log,re.S) if 'Test Passed.' in body and 'Test Failed.' not in body}
for line in s.splitlines():
 if re.match(r'^\| (?:G|B|IO|SW|P)-\d\d \|',line):
  tests=re.findall(r'`(dsp\.[^`]+)`',line); assert tests,line
  assert all(n in names and n in passed for n in tests),(line, "test missing or not passed")
for link in re.findall(r'\[[^\]]*\]\(([^)]+)\)',s):
 if '://' not in link and not link.startswith('#'): assert (m.parent/link.split('#')[0]).exists(),link
print(f'Validated {len(ids)} requirements against {len(names)} discovered and {len(passed)} successful CTest cases.')

xml=root/'build/docs/xml/index.xml'
if xml.exists():
 import xml.etree.ElementTree as ET
 types={c.findtext('name').rsplit('::',1)[-1] for c in ET.parse(xml).getroot().findall('compound') if c.get('kind') in ('class','struct')}
 assert {'Node','DAG','ExecutionPlan','PlanSwitcher','BufferPool','DelayLine','ProcessContext'} <= types
 print('Doxygen XML includes all core public owner/context types.')
