#!/usr/bin/env python3
import re
from pathlib import Path
from dataclasses import dataclass
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

WIN = 1000

LOGDIR = Path("logs")
OUTDIR = Path("plots")

HEX8 = re.compile(r"[0-9A-Fa-f]{8}")
RE_ACT = re.compile(r"^\[ACT\s+\]\s+seq=([0-9A-Fa-f]{8})\s+m=\(([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8})\)\s+sat=([0-9A-Fa-f]{8})")
# [GDB-SEU] seq=50 flip bit=12 flips=3
RE_GDB = re.compile(r"^\[GDB-SEU\]\s+seq=(\d+)\s+flip\s+bit=(\d+)\s+flips=(\d+)")
# [COST ] protect_mode=00000001 tmr_calls=00004E20 srl_calls=00000000 srl_clamps=00000000
RE_COST = re.compile(r"^\[COST\s+\]\s+protect_mode=([0-9A-Fa-f]{8})\s+tmr_calls=([0-9A-Fa-f]{8})\s+srl_calls=([0-9A-Fa-f]{8})\s+srl_clamps=([0-9A-Fa-f]{8})")

LINE_STYLES = {
    "saturation": {
        "baseline (no SEU)": dict(linestyle="-",  linewidth=2.0),
        "SEU in prev":       dict(linestyle="-", linewidth=1.6),
        "SEU in curr":       dict(linestyle="-", linewidth=1.6),
        "SEU in cmd":        dict(linestyle="--", linewidth=1.8),
    }
}


def u32(x: int) -> int:
    return x & 0xFFFFFFFF

def i32_from_u32(x: int) -> int:
    x = u32(x)
    return x - 0x100000000 if (x & 0x80000000) else x

@dataclass(frozen=True)
class Actuator:
    seq: int
    mx: int
    my: int
    mz: int
    sat_flags: int
    amax: int
    dm: int

@dataclass(frozen=True)
class Gdb:
    seq: int
    bit: int
    flips_counter: int

@dataclass(frozen=True)
class Cost:
    protect_mode: int
    tmr_calls: int
    srl_calls: int
    srl_clamps: int


def require(path: Path) -> Path:
    if not path.exists():
        raise SystemExit(f"ERROR: missing file: {path}")
    return path

def parse_act_samples(path: Path) -> dict[int, Actuator]:
    samples: dict[int, Actuator] = {}
    prev_m = None

    with require(path).open("r", errors="ignore") as f:
        for line in f:
            if not RE_ACT.match(line):
                continue
            nums = HEX8.findall(line)
            if len(nums) < 5:
                continue

            seq = int(nums[0], 16)
            mx = i32_from_u32(int(nums[1], 16))
            my = i32_from_u32(int(nums[2], 16))
            mz = i32_from_u32(int(nums[3], 16))
            sat = int(nums[4], 16)

            amax = max(abs(mx), abs(my), abs(mz))

            if prev_m is None:
                dm = 0
            else:
                pmx, pmy, pmz = prev_m
                dm = max(abs(mx - pmx), abs(my - pmy), abs(mz - pmz))
            prev_m = (mx, my, mz)

            samples[seq] = Actuator(seq, mx, my, mz, sat, amax, dm)

    if not samples:
        raise SystemExit(f"ERROR: no [ACT] samples parsed from {path}")
    return samples

def parse_gdb_flips(path: Path) -> list[Gdb]:
    flips: list[Gdb] = []
    if not path.exists():
        return flips
    with path.open("r", errors="ignore") as f:
        for line in f:
            m = RE_GDB.match(line.strip())
            if not m:
                continue
            flips.append(Gdb(seq=int(m.group(1)), bit=int(m.group(2)), flips_counter=int(m.group(3))))
    return flips

def parse_cost(path: Path) -> Cost | None:
    if not path.exists():
        return None
    with path.open("r", errors="ignore") as f:
        for line in f:
            m = RE_COST.match(line.strip())
            if not m:
                continue
            return Cost(
                protect_mode=int(m.group(1), 16),
                tmr_calls=int(m.group(2), 16),
                srl_calls=int(m.group(3), 16),
                srl_clamps=int(m.group(4), 16),
            )
    return None

def window_counts(seqs, flags, win):
    bins: dict[int, int] = {}
    for s, fl in zip(seqs, flags):
        if s == 0:
            continue
        end = ((s + win - 1) // win) * win
        bins[end] = bins.get(end, 0) + int(fl)
    xs = sorted(bins.keys())
    ys = [bins[x] for x in xs]
    return xs, ys

def plot_saturation_rate(runs: dict[str, dict[int, Actuator]], out_png: Path):
    plt.figure(figsize=(12, 5))

    for name, smp in runs.items():
        seqs = sorted(smp.keys())
        flags = [1 if smp[s].sat_flags else 0 for s in seqs]
        xs, ys = window_counts(seqs, flags, WIN)
        style = LINE_STYLES["saturation"].get(name, {})
        plt.plot(xs, ys, label=name, **style)

    ax = plt.gca()
    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel(f"saturation events per window (win={WIN})")
    plt.title("Saturation rate per window")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def plot_pre_amplitude_all(runs: dict[str, dict[int, Actuator]], flips: dict[str, list[Gdb]], out_png: Path):
    plt.figure(figsize=(16, 5))
    
    for name, smp in runs.items():
        seqs = sorted(smp.keys())
        ys = [smp[s].amax for s in seqs]
        plt.plot(seqs, ys, label=name, zorder=2)

    plt.ylim(8, None)
    ax = plt.gca()
    ax.set_yscale("log")

    ymin, ymax = plt.ylim()
    for fs in flips.values():
        for f in fs:
            plt.vlines(f.seq, ymin, ymax, linewidth=0.4, alpha=0.08)

    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel("A(seq)=max(|mx|,|my|,|mz|)")
    plt.title("Control amplitude")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()

def plot_curr_window_compare(curr, curr_tmr, out_png: Path):
    plt.figure(figsize=(12, 5))

    def series(label, smp):
        seqs = sorted(smp.keys())
        flags = [1 if smp[s].sat_flags else 0 for s in seqs]

        xs, ys = window_counts(seqs, flags, WIN)
        plt.plot(xs, ys, label=label)

    series("curr (without protection)", curr)
    series("curr + TMR", curr_tmr)

    ax = plt.gca()
    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel(f"saturation events per window (win={WIN})")
    plt.title("curr: saturation rate per window (before vs after TMR)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()

def plot_curr_mismatch_compare(baseline, curr, curr_tmr, out_png: Path):
    plt.figure(figsize=(12, 5))

    def series(label, smp):
        xs, ys = mismatch_count_vs_baseline(baseline, smp, WIN)
        plt.plot(xs, ys, label=label)

    series("curr (without protection)", curr)
    series("curr + TMR", curr_tmr)

    ax = plt.gca()
    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel(f"mismatch count vs baseline (win={WIN})")
    plt.title("curr: mismatch vs baseline (before vs after TMR)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def plot_cmd_amplitude_compare(cmd, cmd_srl, flips_cmd, flips_srl, out_png: Path):
    plt.figure(figsize=(12, 5))

    def plot_line(label, smp):
        seqs = sorted(smp.keys())
        ys = [smp[s].amax for s in seqs]
        plt.plot(seqs, ys, label=label, zorder=2)

    plot_line("cmd (without protection)", cmd)
    plot_line("cmd + SRL", cmd_srl)

    plt.ylim(8, None)
    ax = plt.gca()
    ax.set_yscale("log")

    ymin, ymax = plt.ylim()
    for f in (flips_cmd + flips_srl):
        plt.vlines(f.seq, ymin, ymax, linewidth=0.4, alpha=0.12)

    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel("A(seq)=max(|mx|,|my|,|mz|)")
    plt.title("cmd: control amplitude (before vs after SRL)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def mismatch_count_vs_baseline(baseline, run, win):
    bins = {}
    for seq, a in run.items():
        if seq == 0 or seq not in baseline:
            continue
        b = baseline[seq]
        mismatch = int((a.mx != b.mx) or (a.my != b.my) or (a.mz != b.mz))
        end = ((seq + win - 1) // win) * win
        bins[end] = bins.get(end, 0) + mismatch

    xs = sorted(bins.keys())
    ys = [bins[x] for x in xs]
    return xs, ys

def plot_mismatch_count(baseline, runs, out_png):
    plt.figure(figsize=(12, 5))
    for name, smp in runs.items():
        xs, ys = mismatch_count_vs_baseline(baseline, smp, WIN)
        plt.plot(xs, ys, label=name)

    ax = plt.gca()
    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel(f"mismatch count vs baseline (win={WIN})")
    plt.title("Mismatch count vs baseline")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()


def compute_cost_metrics(name: str, cost: Cost, n_samples: int, n_seu: int):
    tmr_rate = cost.tmr_calls / n_samples if n_samples else 0.0

    if cost.srl_calls > 0:
        srl_clamps_per_call = cost.srl_clamps / cost.srl_calls
    else:
        srl_clamps_per_call = 0.0

    if n_seu > 0:
        srl_clamps_per_seu = cost.srl_clamps / n_seu
    else:
        srl_clamps_per_seu = 0.0

    return {
        "name": name,
        "protect_mode": cost.protect_mode,
        "n_samples": n_samples,
        "n_seu": n_seu,
        "tmr_calls": cost.tmr_calls,
        "srl_calls": cost.srl_calls,
        "srl_clamps": cost.srl_clamps,
        "tmr_rate": tmr_rate,
        "srl_clamps_per_call": srl_clamps_per_call,
        "srl_clamps_per_seu": srl_clamps_per_seu,
    }

def save_cost_table(metrics: list[dict], out_csv: Path):
    with out_csv.open("w") as f:
        f.write("name,protect_mode,N_samples,N_seu,tmr_calls,srl_calls,srl_clamps,tmr_rate,srl_clamps_per_call,srl_clamps_per_seu\n")
        for m in metrics:
            f.write(f"{m['name']},{m['protect_mode']},{m['n_samples']},{m['n_seu']},{m['tmr_calls']},{m['srl_calls']},{m['srl_clamps']},{m['tmr_rate']:.6f},{m['srl_clamps_per_call']:.6f},{m['srl_clamps_per_seu']:.6f}\n")

def n_samples_from_run(run: dict[int, Actuator]) -> int:
    return max(run.keys()) if run else 0


def main():
    OUTDIR.mkdir(exist_ok=True)

    baseline = parse_act_samples(LOGDIR / "seu_none.log")
    prev     = parse_act_samples(LOGDIR / "seu_mode0.log")
    curr     = parse_act_samples(LOGDIR / "seu_mode1.log")
    cmd      = parse_act_samples(LOGDIR / "seu_mode2.log")

    curr_tmr = parse_act_samples(LOGDIR / "seu_mode1_sift.log")
    cmd_srl  = parse_act_samples(LOGDIR / "seu_mode2_sift.log")

    flips_prev     = parse_gdb_flips(LOGDIR / "gdb_mode0.log")
    flips_curr     = parse_gdb_flips(LOGDIR / "gdb_mode1.log")
    flips_cmd      = parse_gdb_flips(LOGDIR / "gdb_mode2.log")
    flips_curr_tmr = parse_gdb_flips(LOGDIR / "gdb_mode1_sift.log")
    flips_cmd_srl  = parse_gdb_flips(LOGDIR / "gdb_mode2_sift.log")

    pre_runs = {
        "baseline (no SEU)": baseline,
        "SEU in prev": prev,
        "SEU in curr": curr,
        "SEU in cmd": cmd,
    }

    pre_flips = {
        "prev": flips_prev,
        "curr": flips_curr,
        "cmd": flips_cmd,
    }

    plot_pre_amplitude_all(pre_runs, pre_flips, OUTDIR / "pre_amplitude_all.png")
    plot_saturation_rate(pre_runs,   OUTDIR / "pre_sat_rate_all.png")
    plot_mismatch_count(baseline, {"SEU in prev": prev, "SEU in curr": curr, "SEU in cmd": cmd}, OUTDIR / "mismatch_count_vs_baseline.png")

    plot_curr_window_compare(curr, curr_tmr,   OUTDIR / "curr_sat_rate_compare.png")
    plot_cmd_amplitude_compare(cmd, cmd_srl, flips_cmd, flips_cmd_srl, OUTDIR / "cmd_amplitude_compare.png")
    plot_curr_mismatch_compare(baseline, curr, curr_tmr, OUTDIR / "curr_mismatch_compare.png")

    scenarios = [
        ("none", baseline, LOGDIR / "seu_none.log", []),
        ("prev", prev, LOGDIR / "seu_mode0.log", flips_prev),
        ("curr", curr, LOGDIR / "seu_mode1.log", flips_curr),
        ("cmd", cmd, LOGDIR / "seu_mode2.log", flips_cmd),
        ("curr_tmr", curr_tmr, LOGDIR / "seu_mode1_sift.log", flips_curr_tmr),
        ("cmd_srl", cmd_srl,  LOGDIR / "seu_mode2_sift.log", flips_cmd_srl),
    ]

    cost_metrics = []
    for name, run, cost_log, flips in scenarios:
        c = parse_cost(cost_log)
        if not c:
            continue

        n_samples = n_samples_from_run(run)
        n_seu = len(flips)

        cost_metrics.append(compute_cost_metrics(name, c, n_samples, n_seu))

    if cost_metrics:
        save_cost_table(cost_metrics, OUTDIR / "cost_metrics.csv")


if __name__ == "__main__":
    main()
