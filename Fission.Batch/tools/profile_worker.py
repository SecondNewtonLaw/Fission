import argparse
import collections
import ctypes
import hashlib
import json
from pathlib import Path
import re
import statistics

from check_worker import Worker, comparable, profiles, records, request, sha


def cpu(process):
    values = [ctypes.c_ulonglong() for _ in range(4)]
    get_times = ctypes.WinDLL("kernel32", use_last_error=True).GetProcessTimes
    get_times.argtypes = [ctypes.c_void_p, *([ctypes.POINTER(ctypes.c_ulonglong)] * 4)]
    if not get_times(int(process._handle), *(ctypes.byref(v) for v in values)):
        raise ctypes.WinError(ctypes.get_last_error())
    return (values[2].value + values[3].value) / 10_000_000


def measure(worker, inputs, identifier):
    before = cpu(worker.process)
    events, wall, sent, received = worker.request(request(inputs, request_id=identifier))
    consumed = cpu(worker.process) - before
    rows = records(events)
    assert all("ir" not in row for row in rows), "--no-ir emitted IR"
    stages = collections.Counter()
    rewrites = collections.Counter()
    for row in rows:
        for name, seconds in re.findall(r"^\t([^\t].*?): ([0-9.e+-]+)s", row["timingBreakdown"], re.M):
            stages[name] += float(seconds)
        for name, seconds in re.findall(r"^\t\t([^\t].*?): ([0-9.e+-]+)s", row["timingBreakdown"], re.M):
            rewrites[name] += float(seconds)
    measured = dict(wall_seconds=wall, cpu_seconds=consumed, records=len(rows), failures=events[-1]["failures"],
                    compile_seconds=sum(r["timings"]["compileSeconds"] for r in rows),
                    decompile_seconds=sum(r["timings"]["decompileSeconds"] for r in rows),
                    compile_calls=sum(not r["compileReused"] for r in rows),
                    compile_reuses=sum(r["compileReused"] for r in rows),
                    request_bytes=sent, response_bytes=received, stages=dict(stages), rewrites=dict(rewrites))
    return rows, measured


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    paths = sorted(args.corpus.glob("*.lua"), key=lambda p: (-p.stat().st_size, p.name))
    selected = paths[:4]
    for ceiling in (70_000, 50_000, 30_000, 15_000):
        selected.append(next(p for p in paths if p.stat().st_size < ceiling and p not in selected))
    inputs = [dict(id=p.stem, path=p.as_posix(), source=p.read_bytes().decode("utf-8")) for p in selected]
    result = dict(before=str(args.before.resolve()), after=str(args.after.resolve()),
                  before_sha256=sha(args.before), after_sha256=sha(args.after), profiles=profiles(),
                  sources=[dict(path=str(p), bytes=p.stat().st_size, sha256=sha(p)) for p in selected], runs=[])
    workers = {"before": Worker(args.before.resolve()), "after": Worker(args.after.resolve())}
    warmup = request(inputs[:1], [dict(profiles()[0], inputIds=[])], request_id="warmup")
    for worker in workers.values():
        records(worker.request(warmup)[0])
    try:
        for repeat in range(args.repeats):
            order = ("before", "after") if repeat % 2 == 0 else ("after", "before")
            outputs, run = {}, dict(repeat=repeat + 1, order=order)
            for name in order:
                outputs[name], run[name] = measure(workers[name], inputs, f"{name}-{repeat}")
                print(name, repeat + 1, json.dumps({k: v for k, v in run[name].items() if k not in ("stages", "rewrites")}), flush=True)
            assert [comparable(r) for r in outputs["before"]] == [comparable(r) for r in outputs["after"]], "output drift"
            assert [(r["id"], r["profileId"], r["path"], r["compileReused"]) for r in outputs["before"]] == [
                (r["id"], r["profileId"], r["path"], r["compileReused"]) for r in outputs["after"]], "protocol drift"
            run["output_sha256"] = hashlib.sha256(json.dumps([comparable(r) for r in outputs["after"]], sort_keys=True).encode()).hexdigest()
            result["runs"].append(run)
            args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        result["medians"] = {name: {key: statistics.median(r[name][key] for r in result["runs"])
                                    for key in ("wall_seconds", "cpu_seconds", "compile_seconds", "decompile_seconds")}
                             for name in workers}
        result["speedup"] = {key: result["medians"]["before"][key] / result["medians"]["after"][key]
                             for key in ("wall_seconds", "cpu_seconds", "decompile_seconds")}
    finally:
        for worker in workers.values():
            assert worker.close() == b"", "worker stderr"
    assert sha(args.before) == result["before_sha256"]
    assert sha(args.after) == result["after_sha256"]
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("speedup", result["speedup"], flush=True)


if __name__ == "__main__":
    main()
