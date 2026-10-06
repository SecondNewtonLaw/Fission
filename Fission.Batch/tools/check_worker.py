import argparse
import copy
import hashlib
import json
from pathlib import Path
import statistics
import subprocess
import tempfile
import threading
import time


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def encode(value):
    return (json.dumps(value, ensure_ascii=True, separators=(",", ":")) + "\n").encode()


class Worker:
    def __init__(self, executable):
        self.stderr = tempfile.TemporaryFile()
        self.start = time.perf_counter()
        self.process = subprocess.Popen([str(executable), "--worker"], stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.stderr)

    def request(self, value):
        payload = value if isinstance(value, bytes) else encode(value)
        self.timer = threading.Timer(300, self.process.kill)
        self.timer.daemon = True
        self.timer.start()
        start = time.perf_counter()
        self.process.stdin.write(payload)
        self.process.stdin.flush()
        events, size = [], 0
        while True:
            line = self.process.stdout.readline()
            assert line, ("worker exited before terminal event", self.process.poll())
            size += len(line)
            event = json.loads(line)
            events.append(event)
            if event["event"] in ("error", "complete"):
                self.timer.cancel()
                return events, time.perf_counter() - start, len(payload), size

    def close(self):
        self.process.stdin.close()
        assert self.process.wait(timeout=15) == 0
        assert not self.process.stdout.read(), "unexpected records after terminal event"
        self.timer.cancel()
        self.stderr.seek(0)
        stderr = self.stderr.read()
        self.stderr.close()
        return stderr


def profiles():
    flags = {"base": [], "no_names": ["--no-names"], "types": ["--types"],
             "roblox_types": ["--types", "--roblox-types"], "recover_inline": ["--recover-inline"]}
    return [{"id": f"o{o}d{d}_{name}", "optimizationLevel": o, "debugLevel": d,
             "flags": ["--no-comments", "--no-ir", *extra]}
            for o in range(3) for d in range(3) for name, extra in flags.items()
            if name != "recover_inline" or (o == 2 and d > 0)]


def request(inputs, selected=None, request_id="check", budget=120000):
    return {"requestId": request_id, "kind": "source", "budgetMs": budget,
            "inputs": inputs, "profiles": profiles() if selected is None else selected}


def comparable(record):
    ignored = {"event", "requestId", "id", "profileId", "compileReused", "timings", "timingBreakdown", "path"}
    if record["status"] == "failed_to_compile":
        return {key: record[key] for key in ("input", "status", "error")}
    return {key: value for key, value in record.items() if key not in ignored}


def cli(executable, paths, profile):
    start = time.perf_counter()
    result = subprocess.run([str(executable), "--opt", str(profile["optimizationLevel"]), "--debug",
                             str(profile["debugLevel"]), *profile["flags"], *map(str, paths)],
                            capture_output=True, timeout=180)
    wall = time.perf_counter() - start
    assert result.returncode in (0, 1), result.stderr.decode(errors="replace")
    return [json.loads(line) for line in result.stdout.splitlines()], wall, len(result.stdout), len(result.stderr)


def records(events):
    assert events[-1]["event"] == "complete", events[-1]
    rows = events[:-1]
    assert all(row["event"] == "record" for row in rows)
    assert events[-1]["records"] == len(rows)
    assert events[-1]["failures"] == sum(row["status"] != "success" for row in rows)
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--worker", type=Path, required=True)
    parser.add_argument("--legacy", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    baseline_hash = sha(args.legacy)
    result = {"worker": str(args.worker.resolve()), "worker_sha256": sha(args.worker),
              "legacy": str(args.legacy.resolve()), "legacy_sha256": baseline_hash,
              "benchmark_baseline": "same-build executable in existing file CLI mode",
              "protected_binary_drift": []}
    fixtures = [root / name for name in ("Samples/NestedTables.lua", "Samples/NestedCalls.lua",
                "Samples/StressTests/01_numeric_for.lua", "Samples/StressTests/09_closures.lua",
                "Samples/StressTests/12_methods_and_tables.lua", "Samples/StressTests/14_expressions.lua")]
    with tempfile.TemporaryDirectory(prefix="fission-worker-check-") as directory:
        extras = {"invalid.lua": b"local = broken", "utf8.lua": 'local s = "caf\u00e9 \u96ea"\r\nreturn s\r\n'.encode(),
                  "nul.lua": b'return "a\x00b"', "inline.lua": b"local function add(x) return x+1 end\nreturn add(3)",
                  "empty.lua": b""}
        for name, data in extras.items():
            path = Path(directory) / name
            path.write_bytes(data)
            fixtures.append(path)
        inputs = [{"id": str(i), "path": path.as_posix(), "source": path.read_bytes().decode("utf-8")}
                  for i, path in enumerate(fixtures)]
        worker = Worker(args.worker)
        events, _, _, _ = worker.request(request(inputs))
        actual = {(row["profileId"], row["id"]): row for row in records(events)}
        comparisons = 0
        for profile in profiles():
            expected, _, _, _ = cli(args.worker, fixtures, profile)
            protected, _, _, _ = cli(args.legacy, fixtures, profile)
            by_path = {row["path"]: row for row in expected}
            protected_by_path = {row["path"]: row for row in protected}
            for i, path in enumerate(fixtures):
                row = actual[profile["id"], str(i)]
                assert row["input"] == inputs[i]["source"] and row["path"] == inputs[i]["path"]
                assert comparable(row) == comparable(by_path[path.as_posix()]), (profile["id"], str(path))
                protected_row = protected_by_path[path.as_posix()]
                if comparable(row) != comparable(protected_row):
                    result["protected_binary_drift"].append({"profile": profile["id"], "path": str(path),
                        "changed_fields": [key for key in comparable(row) if comparable(row).get(key) != comparable(protected_row).get(key)]})
                comparisons += 1
            print("parity", profile["id"], flush=True)
        events2, _, _, _ = worker.request(request(inputs, request_id="repeat"))
        assert [comparable(row) for row in records(events2)] == [comparable(row) for row in records(events)]
        capture = {"id": "captures", "optimizationLevel": 1, "debugLevel": 1, "flags": ["--ast", "--cfg", "--notes"]}
        capture_rows = records(worker.request(request(inputs[:1], [capture]))[0])
        capture_expected = cli(args.worker, fixtures[:1], capture)[0][0]
        assert comparable(capture_rows[0]) == comparable(capture_expected)
        assert all(field in capture_rows[0] for field in ("ast", "cfg", "debugNotes", "ir"))
        base = profiles()[0]
        partial = [dict(base, inputIds=["1", "0"]), dict(base, id="empty", inputIds=[])]
        partial_rows = records(worker.request(request(inputs, partial))[0])
        assert [row["id"] for row in partial_rows] == ["1", "0"]
        duplicate = [dict(inputs[0], id="first"), dict(inputs[0], id="duplicate")]
        duplicate_rows = records(worker.request(request(duplicate, [base]))[0])
        assert [row["compileReused"] for row in duplicate_rows] == [False, True]
        changed = [dict(inputs[0], source="return 987654321")]
        changed_row = records(worker.request(request(changed, [base]))[0])[0]
        assert "987654321" in changed_row["output"] and not changed_row["compileReused"]
        invalid = [b"{\n", b"[]\n"]
        valid = request(inputs[:1], [base])
        for key, value in (("kind", "bytecode"), ("budgetMs", 0), ("budgetMs", -1), ("budgetMs", 1.5),
                           ("budgetMs", 2147483648), ("inputs", []), ("profiles", []), ("unexpected", True)):
            invalid.append(dict(valid, **{key: value}))
        for key, value in (("optimizationLevel", -1), ("optimizationLevel", 3), ("debugLevel", 3),
                           ("optimizationLevel", 1.0), ("flags", ["--unknown"]), ("flags", ["--out"]),
                           ("inputIds", ["missing"]), ("inputIds", ["0", "0"]), ("unexpected", True)):
            invalid.append(dict(valid, profiles=[dict(base, **{key: value})]))
        invalid.extend([dict(valid, inputs=[inputs[0], inputs[0]]), dict(valid, profiles=[base, base]),
                        dict(valid, inputs=[dict(inputs[0], source=42)]), dict(valid, requestId=42)])
        for bad in invalid:
            error = worker.request(bad)[0]
            assert len(error) == 1 and error[0]["event"] == "error", error
            if isinstance(bad, dict) and isinstance(bad.get("requestId"), str):
                assert error[0]["requestId"] == bad["requestId"]
        recovered = records(worker.request(valid)[0])
        assert recovered[0]["status"] == "success"
        long_source = "local t={}\n" + "\n".join(f"t[{i}]={i}+math.random()" for i in range(2500)) + "\nreturn t"
        timeout_inputs = [{"id": "slow", "path": "virtual/0slow.lua", "source": long_source},
                          {"id": "next", "path": "virtual/1next.lua", "source": "return 42"}]
        timed = records(worker.request(request(timeout_inputs, [dict(base, flags=["--no-ir", "--notes"])], budget=1))[0])
        assert timed[0]["status"] == "failed_to_decompile" and "time budget" in timed[0].get("debugNotes", ""), timed[0]["status"]
        assert timed[1]["status"] == "success", timed[1]
        assert worker.close() == b"", "unexpected verification stderr"
        result["verification"] = {"cli_comparisons": comparisons + 1, "profiles": len(profiles()),
                                  "fixtures": len(fixtures), "repeat_records": len(actual),
                                  "invalid_requests": len(invalid), "partial_selection": True,
                                  "exact_utf8_crlf_nul_echo": True, "duplicate_source_reuse": True,
                                  "changed_source_same_id": True, "timeout_then_success": True,
                                  "stderr_bytes": 0, "eof_exit_code": 0}

    corpus_paths = sorted(args.corpus.glob("*.lua"))[:32]
    assert len(corpus_paths) == 32
    corpus = [{"id": path.stem, "path": path.as_posix(), "source": path.read_bytes().decode("utf-8")}
              for path in corpus_paths]
    result["corpus"] = {"directory": str(args.corpus), "selection": "first 32 SHA-sorted .lua files, no size filtering",
                        "sources": 32, "source_bytes": sum(len(item["source"].encode()) for item in corpus),
                        "source_sha256": [hashlib.sha256(item["source"].encode()).hexdigest() for item in corpus]}
    startup = {"legacy_help_seconds": [], "worker_handshake_seconds": []}
    noop = request(corpus[:1], [dict(profiles()[0], inputIds=[])], request_id="startup")
    for _ in range(7):
        start = time.perf_counter()
        subprocess.run([str(args.worker), "--help"], capture_output=True, timeout=30)
        startup["legacy_help_seconds"].append(time.perf_counter() - start)
        worker = Worker(args.worker)
        records(worker.request(noop)[0])
        startup["worker_handshake_seconds"].append(time.perf_counter() - worker.start)
        assert worker.close() == b""
    result["startup"] = startup
    runs = []
    worker = Worker(args.worker)
    for repeat in range(args.repeats):
        sample = {"repeat": repeat + 1, "legacy_wall_seconds": 0, "legacy_compile_seconds": 0,
                  "legacy_decompile_seconds": 0, "legacy_stdout_bytes": 0, "legacy_stderr_bytes": 0}
        expected = {}
        for profile in profiles():
            rows, wall, stdout_bytes, stderr_bytes = cli(args.worker, corpus_paths, profile)
            sample["legacy_wall_seconds"] += wall
            sample["legacy_stdout_bytes"] += stdout_bytes
            sample["legacy_stderr_bytes"] += stderr_bytes
            for row in rows:
                expected[profile["id"], Path(row["path"]).stem] = row
                sample["legacy_compile_seconds"] += row.get("timings", {}).get("compileSeconds", 0)
                sample["legacy_decompile_seconds"] += row.get("timings", {}).get("decompileSeconds", 0)
        events, wall, sent, received = worker.request(request(corpus, request_id=f"bench-{repeat}"))
        rows = records(events)
        for row in rows:
            assert comparable(row) == comparable(expected[row["profileId"], row["id"]]), (row["profileId"], row["id"])
        sample.update(worker_wall_seconds=wall, worker_compile_seconds=sum(row["timings"]["compileSeconds"] for row in rows),
                      worker_decompile_seconds=sum(row["timings"]["decompileSeconds"] for row in rows),
                      worker_request_bytes=sent, worker_response_bytes=received, records=len(rows),
                      failures=events[-1]["failures"], compile_calls=sum(not row["compileReused"] for row in rows),
                      compile_reuses=sum(row["compileReused"] for row in rows))
        runs.append(sample)
        print("benchmark", json.dumps(sample), flush=True)
    assert worker.close() == b"", "unexpected benchmark stderr"
    result["runs"] = runs
    result["medians"] = {key: statistics.median(run[key] for run in runs) for key in runs[0] if key != "repeat"}
    result["medians"]["speedup"] = result["medians"]["legacy_wall_seconds"] / result["medians"]["worker_wall_seconds"]
    assert sha(args.legacy) == baseline_hash, "legacy executable changed"
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("evidence", args.out, flush=True)


if __name__ == "__main__":
    main()
