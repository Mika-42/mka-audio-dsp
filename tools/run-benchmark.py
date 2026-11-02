#!/usr/bin/env python3
import argparse,hashlib,json,os,pathlib,platform,re,subprocess,sys
parser=argparse.ArgumentParser();parser.add_argument('--build',type=pathlib.Path,default=pathlib.Path('build/bench'));parser.add_argument('--output',type=pathlib.Path,default=pathlib.Path('build/performance'));a=parser.parse_args()
root=pathlib.Path(__file__).resolve().parents[1];a.output.mkdir(parents=True,exist_ok=True)
cache=(a.build/'CMakeCache.txt').read_text(); compiler_file=next((a.build/'CMakeFiles').glob('*/CMakeCXXCompiler.cmake')); compiler=re.search(r'set\(CMAKE_CXX_COMPILER \"([^\"]+)\"\)',compiler_file.read_text()).group(1)
cpu=next((s.split(':',1)[1].strip() for s in pathlib.Path('/proc/cpuinfo').read_text().splitlines() if s.startswith('model name')),'unknown')
env={'os':platform.platform(),'cpu':cpu,'allowed_cpus':len(os.sched_getaffinity(0)),'cgroup_cpu_max':pathlib.Path('/sys/fs/cgroup/cpu.max').read_text().strip(),
 'compiler':subprocess.check_output([compiler,'--version'],text=True).splitlines()[0],'flags':[s for s in cache.splitlines() if s.startswith(('CMAKE_CXX_FLAGS:','CMAKE_CXX_FLAGS_RELEASE:','CMAKE_BUILD_TYPE:'))],
 'sample_rate':48000,'warmup_callbacks':20,'measured_callbacks_per_scenario':100,'timer':'std::chrono::steady_clock around exec, outside RT probe','quantiles':'nearest rank ceil(q*N)-1',
 'load_before':os.getloadavg(),'condition':'shared cloud container; tight loop; CSV between callbacks outside timing; no CPU pinning or realtime priority',
 'core_base_commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
 'harness_sha256':hashlib.sha256((root/'bench/v1_benchmark.cpp').read_bytes()+(root/'bench/rt_probe.hpp').read_bytes()).hexdigest()}
subprocess.run([str((a.build/'mka-v1-benchmark').resolve()),str(a.output.resolve())],check=True)
env['load_after']=os.getloadavg();(a.output/'environment.json').write_text(json.dumps(env,indent=2)+'\n')
subprocess.run([sys.executable,str(root/'tools/validate-benchmark.py'),str(a.output)],check=True)
