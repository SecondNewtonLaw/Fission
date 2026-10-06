import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import queue
import subprocess
import threading
import time

from check_worker import comparable


class Basic(c.Structure):
    _fields_ = [("user", c.c_int64), ("job", c.c_int64), ("flags", w.DWORD),
                ("minimum", c.c_size_t), ("maximum", c.c_size_t), ("active", w.DWORD),
                ("affinity", c.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD)]


class Limits(c.Structure):
    _fields_ = [("basic", Basic), ("io", c.c_uint64 * 6), ("process_memory", c.c_size_t),
                ("job_memory", c.c_size_t), ("process_peak", c.c_size_t), ("job_peak", c.c_size_t)]


class Memory(c.Structure):
    _fields_ = [("cb", w.DWORD), ("faults", w.DWORD), ("peak_ws", c.c_size_t),
                ("ws", c.c_size_t), ("peak_paged", c.c_size_t), ("paged", c.c_size_t),
                ("peak_nonpaged", c.c_size_t), ("nonpaged", c.c_size_t),
                ("pagefile", c.c_size_t), ("peak_pagefile", c.c_size_t), ("private", c.c_size_t)]


kernel = c.WinDLL("kernel32", use_last_error=True)
kernel.CreateJobObjectW.argtypes = [c.c_void_p, w.LPCWSTR]
kernel.CreateJobObjectW.restype = w.HANDLE
kernel.SetInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
kernel.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.K32GetProcessMemoryInfo.argtypes = [w.HANDLE, c.c_void_p, w.DWORD]


def checked(ok):
    if not ok:
        raise c.WinError(c.get_last_error())


def memory(child):
    m = Memory()
    m.cb = c.sizeof(m)
    if kernel.K32GetProcessMemoryInfo(int(child._handle), c.byref(m), m.cb):
        return {"private": m.private, "working_set": m.ws, "peak_private": m.peak_pagefile, "peak_working_set": m.peak_ws}
    return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--worker", type=Path, required=True)
    ap.add_argument("--request", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--repeats", type=int, default=1)
    ap.add_argument("--memory-mib", type=int, default=1024)
    ap.add_argument("--seconds", type=float, default=30)
    a = ap.parse_args()
    if a.repeats < 1 or not 64 <= a.memory_mib <= 2048 or not 0 < a.seconds <= 120:
        ap.error("invalid bounded replay limits")
    raw = a.request.read_bytes()
    if len(raw) > 16 * 1024 * 1024:
        ap.error("request exceeds 16 MiB replay limit")
    req = json.loads(raw)
    payload = json.dumps(req, ensure_ascii=True, separators=(",", ":")).encode() + b"\n"
    job = kernel.CreateJobObjectW(None, None)
    checked(job)
    limits = Limits()
    limits.basic.flags = 0x100 | 0x200 | 0x2000
    limits.process_memory = limits.job_memory = a.memory_mib * 1024 * 1024
    checked(kernel.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)))
    child = subprocess.Popen([str(a.worker.resolve()), "--worker"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=0x08000000)
    runs, samples, stop, responses = [], [], threading.Event(), queue.Queue(maxsize=1)
    reason, rows, starts, current, stderr_lines = None, [], [], None, []
    try:
        checked(kernel.AssignProcessToJobObject(job, int(child._handle)))

        def telemetry():
            nonlocal current
            while True:
                line = child.stderr.readline(16 * 1024 * 1024 + 1)
                if not line:
                    return
                try:
                    event = json.loads(line)
                    if event.get("event") != "record-start":
                        raise ValueError("unexpected stderr event")
                    current = event
                    starts.append(event)
                except (ValueError, AttributeError):
                    stderr_lines.append(line.decode(errors="replace"))

        def read():
            try:
                while not stop.is_set():
                    line = child.stdout.readline(16 * 1024 * 1024 + 1)
                    if not line:
                        raise EOFError("worker exited")
                    if len(line) > 16 * 1024 * 1024:
                        raise ValueError("record exceeds 16 MiB replay limit")
                    responses.put(json.loads(line))
            except Exception as e:
                responses.put(e)

        def sample():
            while not stop.wait(.05):
                m = memory(child)
                if m:
                    samples.append(dict(seconds=time.monotonic() - started, **m))

        started = time.monotonic()
        telemetry_thread = threading.Thread(target=telemetry, daemon=True)
        telemetry_thread.start()
        threading.Thread(target=read, daemon=True).start()
        threading.Thread(target=sample, daemon=True).start()
        for i in range(a.repeats):
            timed = threading.Event()

            def timeout():
                timed.set()
                kernel.TerminateJobObject(job, 124)

            timer = threading.Timer(a.seconds, timeout)
            timer.start()
            begin, rows, starts, current, fingerprint = time.monotonic(), [], [], None, hashlib.sha256()
            try:
                child.stdin.write(payload)
                child.stdin.flush()
                while True:
                    event = responses.get(timeout=a.seconds + 1)
                    if isinstance(event, Exception):
                        raise event
                    if event.get("event") == "record-start":
                        assert req.get("reportProgress")
                        current = event
                        starts.append(event)
                        continue
                    if event.get("event") == "complete":
                        break
                    if event.get("event") != "record":
                        raise ValueError(str(event))
                    fingerprint.update(json.dumps(comparable(event), sort_keys=True).encode())
                    rows.append({k: event[k] for k in ("id", "profileId", "status", "timings", "timingBreakdown") if k in event})
                    if "error" in event:
                        rows[-1]["error"] = event["error"]
                    if event["status"] != "success":
                        rows[-1]["diagnostic"] = event.get("error") or event.get("output") or event.get("debugNotes")
                time.sleep(.1)
                r = dict(repeat=i + 1, wall_seconds=time.monotonic() - begin, completion=event,
                         fingerprint=fingerprint.hexdigest(), memory=memory(child), records=rows, starts=list(starts))
                assert len(starts) == (len(rows) if req.get("reportProgress") else 0)
                if starts:
                    assert [(x["id"], x["profileId"]) for x in starts] == [(x["id"], x["profileId"]) for x in rows]
                current = None
                assert event["records"] == len(rows)
                assert event["failures"] == sum(row["status"] != "success" for row in rows)
                if runs:
                    assert r["fingerprint"] == runs[0]["fingerprint"], "output drift across repeated requests"
                runs.append(r)
                print(json.dumps({k: r[k] for k in ("repeat", "wall_seconds", "completion", "memory")}), flush=True)
            finally:
                timer.cancel()
            if timed.is_set():
                raise TimeoutError("hard wall-time limit")
    except Exception as e:
        reason = "TimeoutError: hard wall-time limit" if "timed" in locals() and timed.is_set() else type(e).__name__ + ": " + str(e)
        print(reason, flush=True)
    finally:
        stop.set()
        kernel.TerminateJobObject(job, 125)
        child.wait(timeout=5)
        if "telemetry_thread" in locals():
            telemetry_thread.join(timeout=2)
        kernel.CloseHandle(job)
    report = dict(worker=str(a.worker.resolve()), worker_sha256=hashlib.sha256(a.worker.read_bytes()).hexdigest(),
                  request_sha256=hashlib.sha256(raw).hexdigest(), memory_limit_mib=a.memory_mib,
                  request_wall_limit_seconds=a.seconds, exit_code=child.returncode, failure=reason,
                  incomplete_records=rows if reason else [], incomplete_starts=starts if reason else [],
                  active_sample=current, stderr_lines=stderr_lines, runs=runs, samples=samples)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(json.dumps(report, indent=2) + "\n")
    if reason:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
