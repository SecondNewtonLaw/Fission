import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import statistics

from check_worker import Worker, comparable, records, sha


def measure(worker, request):
    events, wall, sent, received = worker.request(request)
    rows = records(events)
    stages, parts = collections.defaultdict(float), collections.defaultdict(float)
    for row in rows:
        for line in row.get("timingBreakdown", "").splitlines():
            match = re.fullmatch(r"(\t+)(.*): ([\d.eE+\-]+)s", line)
            if match:
                target = stages if len(match[1]) == 1 else parts
                target[match[2]] += float(match[3])
    fingerprint = hashlib.sha256(json.dumps([comparable(row) for row in rows],
                                           ensure_ascii=True, sort_keys=True).encode()).hexdigest()
    return {"wall_seconds": wall, "decompile_seconds": sum(row.get("timings", {}).get("decompileSeconds", 0) for row in rows),
            "compile_seconds": sum(row.get("timings", {}).get("compileSeconds", 0) for row in rows),
            "request_bytes": sent, "response_bytes": received, "records": len(rows), "failures": events[-1]["failures"],
            "stages": dict(stages), "parts": dict(parts), "output_fingerprint": fingerprint,
            "failed_records": [{k: row[k] for k in ("id", "profileId", "status", "error") if k in row}
                               for row in rows if row["status"] != "success"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--optimized", type=Path, required=True)
    parser.add_argument("--request", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    request = json.loads(args.request.read_bytes())
    workers = {"baseline": Worker(args.baseline.resolve()), "optimized": Worker(args.optimized.resolve())}
    runs, fingerprint = [], None
    try:
        for repeat in range(args.repeats):
            order = ("baseline", "optimized") if repeat % 2 == 0 else ("optimized", "baseline")
            run = {"repeat": repeat + 1}
            for name in order:
                measurement = measure(workers[name], request)
                if fingerprint is None:
                    fingerprint = measurement["output_fingerprint"]
                assert measurement["output_fingerprint"] == fingerprint, (name, repeat, "output drift")
                run[name] = measurement
                print(name, repeat + 1, measurement["wall_seconds"], measurement["decompile_seconds"], flush=True)
            runs.append(run)
    finally:
        stderr = {name: len(worker.close()) for name, worker in workers.items()}
    medians = {}
    for name in workers:
        medians[name] = {key: statistics.median(run[name][key] for run in runs)
                         for key in ("wall_seconds", "decompile_seconds", "compile_seconds", "records", "failures", "request_bytes", "response_bytes")}
        for group in ("stages", "parts"):
            medians[name][group] = {key: statistics.median(run[name][group].get(key, 0) for run in runs)
                                   for key in runs[0][name][group]}
    result = {"baseline_executable": str(args.baseline.resolve()), "baseline_sha256": sha(args.baseline),
              "optimized_executable": str(args.optimized.resolve()), "optimized_sha256": sha(args.optimized),
              "request_sha256": sha(args.request), "input_count": len(request["inputs"]), "profile_count": len(request["profiles"]),
              "output_fingerprint": fingerprint, "stderr_bytes": stderr, "runs": runs, "medians": medians,
              "wall_speedup": medians["baseline"]["wall_seconds"] / medians["optimized"]["wall_seconds"],
              "native_speedup": medians["baseline"]["decompile_seconds"] / medians["optimized"]["decompile_seconds"]}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("speedup", result["wall_speedup"], "native", result["native_speedup"], "stderr", stderr, flush=True)


if __name__ == "__main__":
    main()
