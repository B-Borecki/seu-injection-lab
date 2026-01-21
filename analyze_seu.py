#!/usr/bin/env python3
import re
from pathlib import Path
from dataclasses import dataclass
import matplotlib.pyplot as plt

BASELINE_LOG = Path("seu_none.log")

RUN_LOGS = {
    "prev": Path("seu_mode0.log"),
    "curr": Path("seu_mode1.log"),
    "cmd":  Path("seu_mode2.log"),
}

GDB_LOGS = {
    "prev": Path("gdb_mode0.log"),
    "curr": Path("gdb_mode1.log"),
    "cmd":  Path("gdb_mode2.log"),
}

OUTDIR = Path("plots")

THR_P = 0.95
HOLD_N = 20
SEARCH_CAP = 50
WIN = 1000
SPIKE_THR = 500
SAMPLE_MS = 10

LABELS = {
    "baseline": "without SEU",
    "prev": "prev",
    "curr": "curr",
    "cmd": "cmd",
}

LINESTYLE = {
    "baseline": "-",
    "curr": "-.",
    "prev": "-",
    "cmd": "--",
}


HEX8 = re.compile(r"[0-9A-Fa-f]{8}")
RE_ACT = re.compile(r"^\[ACT\s+\]")
RE_GDB = re.compile(r"^\[GDB-SEU\]\s+seq=(\d+)\s+(\S+)\s+flip\s+bit=(\d+)\s+flips=(\d+)")

def u32(x: int) -> int:
    return x & 0xFFFFFFFF

def i32_from_u32(x: int) -> int:
    x = u32(x)
    return x - 0x100000000 if (x & 0x80000000) else x

@dataclass
class ActSample:
    seq: int
    mx: int
    my: int
    mz: int
    sat_flags: int
    amax: int
    dm: int

@dataclass
class GdbFlip:
    seq: int
    field: str
    bit: int
    flips_counter: int

def parse_act_samples(path: Path) -> dict[int, ActSample]:
    samples: dict[int, ActSample] = {}
    prev_m = None
    with path.open("r", errors="ignore") as f:
        for line in f:
            line = line.strip()
            if not RE_ACT.match(line):
                continue
            nums = HEX8.findall(line)
            if len(nums) < 5:
                continue

            seq = int(nums[0], 16)
            mx  = i32_from_u32(int(nums[1], 16))
            my  = i32_from_u32(int(nums[2], 16))
            mz  = i32_from_u32(int(nums[3], 16))
            sat_flags = int(nums[4], 16)

            amax = max(abs(mx), abs(my), abs(mz))
            if prev_m is None:
                dm = 0
            else:
                pmx, pmy, pmz = prev_m
                dm = max(abs(mx - pmx), abs(my - pmy), abs(mz - pmz))
            prev_m = (mx, my, mz)

            samples[seq] = ActSample(seq, mx, my, mz, sat_flags, amax, dm)
    return samples

def parse_gdb_flips(path: Path) -> list[GdbFlip]:
    flips: list[GdbFlip] = []
    with path.open("r", errors="ignore") as f:
        for line in f:
            line = line.strip()
            m = RE_GDB.match(line)
            if not m:
                continue
            flips.append(GdbFlip(
                seq=int(m.group(1)),
                field=m.group(2),
                bit=int(m.group(3)),
                flips_counter=int(m.group(4))))
    return flips

def percentile(values: list[int], p: float) -> int:
    if not values:
        raise ValueError("percentile on empty list")
    vs = sorted(values)
    if p <= 0:
        return vs[0]
    if p >= 1:
        return vs[-1]
    idx = int(round(p * (len(vs) - 1)))
    return vs[idx]

def window_counts(seqs: list[int], flags: list[int], win: int) -> tuple[list[int], list[int]]:
    bins = {}
    for s, fl in zip(seqs, flags):
        if s == 0:
            continue
        end = ((s + win - 1) // win) * win
        bins[end] = bins.get(end, 0) + int(fl)
    xs = sorted(bins.keys())
    ys = [bins[x] for x in xs]
    return xs, ys

def recovery_times(samples: dict[int, ActSample], flip_seqs: list[int], threshold: int, hold_n: int, search_cap: int):
    am = {s: samp.amax for s, samp in samples.items()}

    def ok_window(start: int) -> bool:
        for k in range(hold_n):
            v = am.get(start + k)
            if v is None or v > threshold:
                return False
        return True

    out = []
    for s0 in flip_seqs:
        rec = None
        for s in range(s0, s0 + search_cap):
            if ok_window(s):
                rec = s - s0
                break
        out.append(rec)
    return out

def plot_amax(out_png: Path, runs: dict[str, dict[int, ActSample]], flips: dict[str, list[GdbFlip]]):
    plt.figure(figsize=(12, 5))
    for name, smp in runs.items():
        seqs = sorted(smp.keys())
        ys = [smp[s].amax for s in seqs]
        plt.plot(seqs, ys, label=LABELS.get(name, name), zorder=2)

    plt.xlabel("seq")
    plt.ylabel("A(seq)=max(|mx|,|my|,|mz|)")
    plt.title("Control amplitude with SEU flip markers")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()

def plot_window(out_png: Path, runs: dict[str, dict[int, ActSample]], win: int, mode: str, spike_thr: int):
    plt.figure(figsize=(12, 5))
    for name, smp in runs.items():
        seqs = sorted(smp.keys())
        if mode == "sat":
            flags = [1 if smp[s].sat_flags != 0 else 0 for s in seqs]
            ylabel = f"sat events per window (win={win})"
            title = f"Saturation rate per window (win={win})"
        elif mode == "spike":
            flags = [1 if smp[s].dm > spike_thr else 0 for s in seqs]
            ylabel = f"spikes per window (dm>{spike_thr}, win={win})"
            title = f"Spike rate per window (dm>{spike_thr}, win={win})"
        else:
            raise ValueError("mode must be sat or spike")

        xs, ys = window_counts(seqs, flags, win)

        from matplotlib.ticker import MultipleLocator

        ax = plt.gca()
        ax.xaxis.set_major_locator(MultipleLocator(2000))

        plt.plot(xs, ys, linestyle=LINESTYLE.get(name, "-"), label=LABELS.get(name, name))

    plt.xlabel("seq")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()

def plot_recovery_cdf_all(out_png: Path,
                          runs: dict[str, dict[int, ActSample]],
                          flips: dict[str, list[GdbFlip]],
                          threshold: int,
                          hold_n: int,
                          search_cap: int,
                          sample_ms: int):
    plt.figure(figsize=(10, 5))

    any_curve = False

    for name, smp in runs.items():
        f = flips.get(name, [])
        if not f:
            continue

        flip_seqs = [x.seq for x in f]
        rec = recovery_times(smp, flip_seqs, threshold=threshold, hold_n=hold_n, search_cap=search_cap)
        good = [r for r in rec if r is not None]
        if not good:
            continue

        xs = sorted([r * sample_ms for r in good])
        ys = [(i + 1) / len(xs) for i in range(len(xs))]

        plt.plot(xs, ys, linestyle=LINESTYLE.get(name, "-"), label=LABELS.get(name, name))
        any_curve = True

    plt.xlabel("recovery time [ms]")
    plt.ylabel("CDF")
    plt.title(f"CDF of recovery time after SEU")
    plt.grid(True, alpha=0.3)
    if any_curve:
        plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def main():
    OUTDIR.mkdir(parents=True, exist_ok=True)

    baseline = parse_act_samples(BASELINE_LOG)
    base_amax = [s.amax for s in baseline.values()]
    if not base_amax:
        raise SystemExit("ERROR: baseline provided but no [ACT] parsed.")
    threshold = percentile(base_amax, THR_P)

    runs: dict[str, dict[int, ActSample]] = {}
    for name, path in RUN_LOGS.items():
        if not path.exists():
            raise SystemExit(f"ERROR: missing run log: {path}")
        runs[name] = parse_act_samples(path)

    flips: dict[str, list[GdbFlip]] = {}
    for name, path in GDB_LOGS.items():
        if not path.exists():
            raise SystemExit(f"ERROR: missing gdb log: {path}")
        flips[name] = parse_gdb_flips(path)

    plot_amax(OUTDIR / "amplitude_with_flips.png", runs, flips)
    runs_for_window = {"baseline": baseline, **runs}
    plot_window(OUTDIR / "sat_rate_window.png", runs_for_window, win=WIN, mode="sat", spike_thr=SPIKE_THR)
    plot_window(OUTDIR / "spike_rate_window.png", runs_for_window, win=WIN, mode="spike", spike_thr=SPIKE_THR)

    for name, smp in runs.items():
        f = flips.get(name, [])
        if not f:
            continue

        flip_seqs = [x.seq for x in f]
        rec = recovery_times(smp, flip_seqs, threshold=threshold, hold_n=HOLD_N, search_cap=SEARCH_CAP)
        good = [r for r in rec if r is not None]

        if good:
            gs = sorted(good)
            med = gs[len(gs)//2]
            p90 = gs[max(0, int(0.9*len(gs))-1)]
            worst = gs[-1]

        plot_recovery_cdf_all(OUTDIR / "recovery_cdf_all.png", runs=runs, flips=flips, threshold=threshold, hold_n=HOLD_N, search_cap=SEARCH_CAP, sample_ms=SAMPLE_MS)


if __name__ == "__main__":
    main()
