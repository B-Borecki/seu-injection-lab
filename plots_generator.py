#!/usr/bin/env python3
import re
from pathlib import Path
from dataclasses import dataclass
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

WIN = 1000
SPIKE_THR = 500

LOGDIR = Path("logs")
OUTDIR = Path("plots")

HEX8 = re.compile(r"[0-9A-Fa-f]{8}")
#RE_ACT = re.compile(r"^\[ACT\s+\]")
RE_ACT = re.compile(
    r"^\[ACT\s+\]\s+seq=([0-9A-Fa-f]{8})\s+m=\(([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8})\)\s+sat=([0-9A-Fa-f]{8})"
)

# [GDB-SEU] seq=50 flip bit=12 flips=3
RE_GDB = re.compile(r"^\[GDB-SEU\]\s+seq=(\d+)\s+flip\s+bit=(\d+)\s+flips=(\d+)")

# [COST ] protect_mode=00000001 tmr_calls=00004E20 srl_calls=00000000 srl_clamps=00000000
RE_COST = re.compile(r"^\[COST\s+\]\s+protect_mode=([0-9A-Fa-f]{8})\s+tmr_calls=([0-9A-Fa-f]{8})\s+srl_calls=([0-9A-Fa-f]{8})\s+srl_clamps=([0-9A-Fa-f]{8})")

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

def plot_pre_window_all(runs: dict[str, dict[int, Actuator]], kind: str, out_png: Path):
    plt.figure(figsize=(12, 5))

    for name, smp in runs.items():
        seqs = sorted(smp.keys())
        if kind == "sat":
            flags = [1 if smp[s].sat_flags else 0 for s in seqs]
            ylabel = f"sat events per window (win={WIN})"
            title = "Saturation rate per window"
        else:
            flags = [1 if smp[s].dm > SPIKE_THR else 0 for s in seqs]
            ylabel = f"spikes per window (dm>{SPIKE_THR}, win={WIN})"
            title = "Spike rate per window"

        xs, ys = window_counts(seqs, flags, WIN)
        plt.plot(xs, ys, label=name)

    ax = plt.gca()
    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel(ylabel)
    plt.title(title)
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

def plot_curr_window_compare(curr, curr_tmr, kind, out_png: Path):
    plt.figure(figsize=(12, 5))

    def series(label, smp):
        seqs = sorted(smp.keys())
        if kind == "sat":
            flags = [1 if smp[s].sat_flags else 0 for s in seqs]
            ylabel = f"sat events per window (win={WIN})"
            title = "curr: saturation rate per window (before vs after TMR)"
        else:
            flags = [1 if smp[s].dm > SPIKE_THR else 0 for s in seqs]
            ylabel = f"spikes per window (dm>{SPIKE_THR}, win={WIN})"
            title = "curr: spike rate per window (before vs after TMR)"

        xs, ys = window_counts(seqs, flags, WIN)
        plt.plot(xs, ys, label=label)
        return ylabel, title

    ylabel, title = series("curr (without protection)", curr)
    series("curr + TMR", curr_tmr)

    ax = plt.gca()
    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel(ylabel)
    plt.title(title)
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()

def plot_cmd_amplitude_compare(baseline, cmd, cmd_srl, flips_cmd, flips_srl, out_png: Path):
    plt.figure(figsize=(12, 5))

    def plot_line(label, smp):
        seqs = sorted(smp.keys())
        ys = [smp[s].amax for s in seqs]
        plt.plot(seqs, ys, label=label, zorder=2)

    plot_line("baseline (no SEU)", baseline)
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


def compute_event_rates(samples: dict[int, Actuator]) -> dict[str, float]:
    seqs = sorted(samples.keys())
    if not seqs:
        return {"spike_rate": 0.0, "sat_rate": 0.0}

    n = len(seqs)
    spike_cnt = 0
    sat_cnt = 0

    for s in seqs:
        a = samples[s]
        if a.dm > SPIKE_THR:
            spike_cnt += 1
        if a.sat_flags != 0:
            sat_cnt += 1

    return {
        "spike_rate": spike_cnt / n,
        "sat_rate": sat_cnt / n,
    }


def clamp_rate(cost: Cost) -> float:
    # SRL działa per oś (3 osie), więc normalizujemy do "ile razy na oś-próbkę"
    if cost.srl_calls <= 0:
        return 0.0
    denom = 3.0 * float(cost.srl_calls)
    return float(cost.srl_clamps) / denom


def plot_costs(costs: dict[str, Cost], out_png: Path):
    # Sensowny koszt z logów to clamp_rate dla SRL
    labels = []
    rates = []
    for k, c in costs.items():
        if c.srl_calls > 0:
            labels.append(k)
            rates.append(clamp_rate(c))

    if not labels:
        return

    x = list(range(len(labels)))

    plt.figure(figsize=(10, 4.5))
    plt.bar(x, rates)
    plt.xticks(x, labels, rotation=15, ha="right")
    plt.ylabel("clamp rate = srl_clamps / (3 * srl_calls)")
    plt.title("SRL: how often protection actually clamps")
    plt.grid(True, alpha=0.3, axis="y")
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()



def plot_benefit_vs_cost(points: list[tuple[str, float, float, float]], out_png: Path):
    # points: (label, cost_x, spike_drop, sat_drop)
    if not points:
        return

    xs = [p[1] for p in points]
    ys = [0.5 * (p[2] + p[3]) for p in points]

    plt.figure(figsize=(10, 4.5))
    plt.scatter(xs, ys)

    for label, x, spike_drop, sat_drop in points:
        y = 0.5 * (spike_drop + sat_drop)
        plt.annotate(label, (x, y), textcoords="offset points", xytext=(6, 6), fontsize=9)

    plt.xlabel("cost proxy: clamp rate")
    plt.ylabel("benefit proxy: mean(spike_drop, sat_drop)")
    plt.title("Benefit vs cost (SRL)")
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()



def main():
    OUTDIR.mkdir(exist_ok=True)

    baseline = parse_act_samples(LOGDIR / "seu_none.log")
    prev     = parse_act_samples(LOGDIR / "seu_mode0.log")
    curr     = parse_act_samples(LOGDIR / "seu_mode1.log")
    cmd      = parse_act_samples(LOGDIR / "seu_mode2.log")

    curr_tmr = parse_act_samples(LOGDIR / "seu_mode1_sefi.log")
    cmd_srl  = parse_act_samples(LOGDIR / "seu_mode2_sefi.log")

    flips_prev     = parse_gdb_flips(LOGDIR / "gdb_mode0.log")
    flips_curr     = parse_gdb_flips(LOGDIR / "gdb_mode1.log")
    flips_cmd      = parse_gdb_flips(LOGDIR / "gdb_mode2.log")
    flips_curr_tmr = parse_gdb_flips(LOGDIR / "gdb_mode1_sefi.log")
    flips_cmd_srl  = parse_gdb_flips(LOGDIR / "gdb_mode2_sefi.log")

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
    plot_pre_window_all(pre_runs, "spike", OUTDIR / "pre_spike_rate_all.png")
    plot_pre_window_all(pre_runs, "sat",   OUTDIR / "pre_sat_rate_all.png")

    plot_curr_window_compare(curr, curr_tmr, "spike", OUTDIR / "curr_spike_rate_compare.png")
    plot_curr_window_compare(curr, curr_tmr, "sat",   OUTDIR / "curr_sat_rate_compare.png")
    plot_cmd_amplitude_compare(baseline, cmd, cmd_srl, flips_cmd, flips_cmd_srl, OUTDIR / "cmd_amplitude_compare.png")

    # --- costs (better) ---
    costs = {}
    c = parse_cost(LOGDIR / "seu_mode2_sefi.log")
    if c: costs["cmd_srl"] = c
    c = parse_cost(LOGDIR / "seu_mode1_sefi.log")
    if c: costs["curr_tmr"] = c  # tu clamp_rate i tak wyjdzie 0 (OK)
    c = parse_cost(LOGDIR / "seu_none.log")
    if c: costs["none"] = c      # też 0 (OK)

    # 1) clamp rate barplot (real info only for SRL)
    plot_costs(costs, OUTDIR / "cost_clamp_rate.png")

    # 2) benefit vs cost scatter (SRL only, bo tylko SRL ma "ingerencję" w logach)
    points = []
    if "cmd_srl" in costs:
        r_before = compute_event_rates(cmd)
        r_after  = compute_event_rates(cmd_srl)

        spike_drop = r_before["spike_rate"] - r_after["spike_rate"]
        sat_drop   = r_before["sat_rate"]   - r_after["sat_rate"]

        points.append(("cmd_srl", clamp_rate(costs["cmd_srl"]), spike_drop, sat_drop))

    plot_benefit_vs_cost(points, OUTDIR / "benefit_vs_cost.png")


if __name__ == "__main__":
    main()
