import argparse
import collections
import ctypes as c
import hashlib
import json
from pathlib import Path
import statistics
import threading

from check_worker import Worker, sha
from check_native_throughput import measure
from check_worker_memory import kernel, Limits, checked, memory
from profile_worker import cpu


def main():
    p = argparse.ArgumentParser()
    for name in ('baseline', 'optimized', 'request', 'out'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--profile-chunk', type=int, default=2)
    p.add_argument('--cpu', type=int, default=20)
    p.add_argument('--memory-mib', type=int, default=1024)
    p.add_argument('--seconds', type=float, default=120)
    a = p.parse_args()
    if a.repeats < 1 or a.profile_chunk < 1 or not 0 <= a.cpu < 64 or not 64 <= a.memory_mib <= 2048 or not 0 < a.seconds <= 120:
        p.error('invalid bounded benchmark limits')
    req = json.loads(a.request.read_bytes())
    ex = {'baseline': a.baseline.resolve(), 'optimized': a.optimized.resolve()}
    out = dict(executables={n: dict(path=str(x), sha256=sha(x)) for n, x in ex.items()},
               request_sha256=sha(a.request), logical_cpu=a.cpu, priority='ABOVE_NORMAL', warmup='full request',
               aggregate_memory_limit_bytes=a.memory_mib * 1048576, overall_wall_limit_seconds=a.seconds,
               profile_chunk=a.profile_chunk, runs=[])
    kernel.SetProcessAffinityMask.argtypes = [c.c_void_p, c.c_size_t]
    kernel.SetPriorityClass.argtypes = [c.c_void_p, c.c_ulong]
    job = kernel.CreateJobObjectW(None, None)
    checked(job)
    lim = Limits()
    lim.basic.flags, lim.basic.active = 0x100 | 0x200 | 0x2000 | 0x8, 2
    lim.process_memory = lim.job_memory = a.memory_mib * 1048576
    checked(kernel.SetInformationJobObject(job, 9, c.byref(lim), c.sizeof(lim)))
    workers = {}
    timer = threading.Timer(a.seconds, lambda: kernel.TerminateJobObject(job, 124))
    timer.start()
    try:
        for name, path in ex.items():
            w = workers[name] = Worker(path)
            checked(kernel.AssignProcessToJobObject(job, int(w.process._handle)))
            checked(kernel.SetProcessAffinityMask(int(w.process._handle), 1 << a.cpu))
            checked(kernel.SetPriorityClass(int(w.process._handle), 0x8000))
        warm = {n: measure(w, dict(req, requestId='warmup')) for n, w in workers.items()}
        assert warm['baseline']['output_fingerprint'] == warm['optimized']['output_fingerprint']
        assert all(x['failures'] == 0 for x in warm.values())
        chunk_fingerprints = {}
        for repeat in range(a.repeats):
            run = {n: dict(decompile_seconds=0, cpu_seconds=0, wall_seconds=0, records=0, failures=0,
                           stages=collections.Counter(), parts=collections.Counter()) for n in ex}
            fingerprints = []
            for start in range(0, len(req['profiles']), a.profile_chunk):
                sub = dict(req, requestId=f'{repeat}-{start}', profiles=req['profiles'][start:start + a.profile_chunk])
                pair = {}
                order = ('baseline', 'optimized') if (repeat + start // a.profile_chunk) % 2 == 0 else ('optimized', 'baseline')
                for name in order:
                    before = cpu(workers[name].process)
                    m = measure(workers[name], sub)
                    m['cpu_seconds'] = cpu(workers[name].process) - before
                    assert m['failures'] == 0
                    for key in ('decompile_seconds', 'cpu_seconds', 'wall_seconds', 'records', 'failures'):
                        run[name][key] += m[key]
                    for key in ('stages', 'parts'):
                        run[name][key].update(m[key])
                    pair[name] = m['output_fingerprint']
                assert pair['baseline'] == pair['optimized'], (repeat, start, 'output drift')
                assert chunk_fingerprints.setdefault(start, pair['baseline']) == pair['baseline']
                fingerprints.append(pair['baseline'])
            run['output_fingerprint'] = hashlib.sha256(json.dumps(fingerprints).encode()).hexdigest()
            out['runs'].append(run)
            print(a.request.stem, repeat + 1, {n: (round(run[n]['decompile_seconds'], 6), round(run[n]['cpu_seconds'], 6)) for n in ex}, flush=True)
        out['memory'] = {n: memory(w.process) for n, w in workers.items()}
        out['medians'] = {}
        for name in ex:
            med = {k: statistics.median(r[name][k] for r in out['runs']) for k in ('decompile_seconds', 'cpu_seconds', 'wall_seconds', 'records', 'failures')}
            for group in ('stages', 'parts'):
                keys = set().union(*(r[name][group] for r in out['runs']))
                med[group] = {k: statistics.median(r[name][group].get(k, 0) for r in out['runs']) for k in keys}
            out['medians'][name] = med
        for w in workers.values():
            assert w.close() == b''
    finally:
        timer.cancel()
        if any(w.process.poll() is None for w in workers.values()):
            kernel.TerminateJobObject(job, 125)
            for w in workers.values():
                w.process.wait(10)
        kernel.CloseHandle(job)
    assert all(sha(path) == out['executables'][name]['sha256'] for name, path in ex.items())
    a.out.write_text(json.dumps(out, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
