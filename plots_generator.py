#!/usr/bin/env python3

import re
from pathlib import Path
from dataclasses import dataclass
import matplotlib.pyplot as plt
from matplotlib.ticker import MultipleLocator

# Rozmiar okna do agregacji (np. liczba zdarzeń saturacji na WIN próbek)
WIN = 1000

# Katalog z logami wejściowymi
LOGDIR = Path("logs")
# Katalog wyjściowy na wykresy i tabelki
OUTDIR = Path("plots")

# Dopasowanie pojedynczej liczby w hex (8 znaków)
HEX8 = re.compile(r"[0-9A-Fa-f]{8}")
# [ACT  ] seq=........ m=(........,........,........) sat=........
RE_ACT = re.compile(
    r"^\[ACT\s+\]\s+seq=([0-9A-Fa-f]{8})\s+m=\(([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8}),([0-9A-Fa-f]{8})\)\s+sat=([0-9A-Fa-f]{8})"
)
# [GDB-SEU] seq=50 flip bit=12 flips=3
RE_GDB = re.compile(r"^\[GDB-SEU\]\s+seq=(\d+)\s+flip\s+bit=(\d+)\s+flips=(\d+)")
# [COST ] protect_mode=........ tmr_calls=........ srl_calls=........ srl_clamps=........
RE_COST = re.compile(
    r"^\[COST\s+\]\s+protect_mode=([0-9A-Fa-f]{8})\s+tmr_calls=([0-9A-Fa-f]{8})\s+srl_calls=([0-9A-Fa-f]{8})\s+srl_clamps=([0-9A-Fa-f]{8})"
)

# Style linii dla wykresu saturacji
LINE_STYLES = {
    "saturation": {
        "baseline (no SEU)": dict(linestyle="-", linewidth=2.0),
        "SEU in input_prev": dict(linestyle="-", linewidth=1.6),
        "SEU in input_curr": dict(linestyle="-", linewidth=1.6),
        "SEU in output_cmd": dict(linestyle="--", linewidth=1.8),
    }
}

# Utnij do zakresu uint32
def u32(x: int) -> int:
    return x & 0xFFFFFFFF

# Zamień uint32 zapisany w logu na int32
def i32_from_u32(x: int) -> int:
    x = u32(x)
    return x - 0x100000000 if (x & 0x80000000) else x

# Próbka z logu [ACT] (komenda aktuatora)
@dataclass(frozen=True)
class Actuator:
    seq: int
    mx: int
    my: int
    mz: int
    sat_flags: int
    amax: int

# Zdarzenie SEU z logu skryptu GDB (gdzie i ile flipów)
@dataclass(frozen=True)
class Gdb:
    seq: int
    bit: int
    flips_counter: int

# Metryki kosztu z logu [COST]
@dataclass(frozen=True)
class Cost:
    protect_mode: int
    tmr_calls: int
    srl_calls: int
    srl_clamps: int

# Parsuj próbki [ACT] do słownika seq -> Actuator
def parse_act_samples(path: Path) -> dict[int, Actuator]:
    samples: dict[int, Actuator] = {}

    with path.open("r", errors="ignore") as f:
        for line in f:
            # Interesują nas tylko linie [ACT]
            if not RE_ACT.match(line):
                continue
            # Wyciągnij 5 liczb hex: seq, mx, my, mz, sat
            nums = HEX8.findall(line)
            if len(nums) < 5:
                continue
            # seq jest hex w logu
            seq = int(nums[0], 16)
            # mx,my,mz zapisane jako uint32 w hex
            mx = i32_from_u32(int(nums[1], 16))
            my = i32_from_u32(int(nums[2], 16))
            mz = i32_from_u32(int(nums[3], 16))
            # sat_flags jako maska bitowa
            sat = int(nums[4], 16)
            # A(seq)=max(|mx|,|my|,|mz|)
            amax = max(abs(mx), abs(my), abs(mz))

            samples[seq] = Actuator(seq, mx, my, mz, sat, amax)
    return samples

# Parsuj log z GDB
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

# Parsuj jedną linię [COST]
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

# Policz liczbę zdarzeń (np. saturacji) w oknach długości win
def window_counts(seqs, flags, win):
    bins: dict[int, int] = {}
    for s, fl in zip(seqs, flags):
        # Pomijamy seq=0, żeby nie psuł pierwszego okna
        if s == 0:
            continue
        # Zaokrąglij do końca okna (np. 1000, 2000, 3000...)
        end = ((s + win - 1) // win) * win
        bins[end] = bins.get(end, 0) + int(fl)
    xs = sorted(bins.keys())
    ys = [bins[x] for x in xs]
    return xs, ys

# Wykres: liczba saturacji w każdym oknie WIN
def plot_saturation_rate(runs: dict[str, dict[int, Actuator]], out_png: Path):
    plt.figure(figsize=(12, 5))

    for name, smp in runs.items():
        seqs = sorted(smp.keys())
        # Zdarzenie saturacji: sat_flags != 0
        flags = [1 if smp[s].sat_flags else 0 for s in seqs]
        xs, ys = window_counts(seqs, flags, WIN)
        style = LINE_STYLES["saturation"].get(name, {})
        plt.plot(xs, ys, label=name, **style)

    # Ustaw podziałkę osi X co 2000
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

# Wykres: amplituda A(seq) dla wszystkich scenariuszy + znaczniki flipów z GDB
def plot_pre_amplitude_all(runs: dict[str, dict[int, Actuator]], flips: dict[str, list[Gdb]], out_png: Path):
    plt.figure(figsize=(16, 5))

    for name, smp in runs.items():
        seqs = sorted(smp.keys())
        ys = [smp[s].amax for s in seqs]
        plt.plot(seqs, ys, label=name, zorder=2)

    # Skala logarytmiczna
    plt.ylim(8, None)
    ax = plt.gca()
    ax.set_yscale("log")

    # Pionowe linie w miejscach, gdzie GDB wstrzyknął flip
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

# Wykres: saturacja w oknach WIN dla curr (bez ochrony) vs curr+TMR
def plot_curr_window_compare(curr, curr_tmr, out_png: Path):
    plt.figure(figsize=(12, 5))

    def series(label, smp):
        seqs = sorted(smp.keys())
        flags = [1 if smp[s].sat_flags else 0 for s in seqs]
        xs, ys = window_counts(seqs, flags, WIN)
        plt.plot(xs, ys, label=label)

    series("input_curr (without protection)", curr)
    series("input_curr + TMR", curr_tmr)

    ax = plt.gca()
    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel(f"saturation events per window (win={WIN})")
    plt.title("input_curr: saturation rate per window (before vs after TMR)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()

# Wykres: mismatch vs baseline w oknach WIN dla curr (bez ochrony) vs curr+TMR
def plot_curr_mismatch_compare(baseline, curr, curr_tmr, out_png: Path):
    plt.figure(figsize=(12, 5))

    def series(label, smp):
        xs, ys = mismatch_count_vs_baseline(baseline, smp, WIN)
        plt.plot(xs, ys, label=label)

    series("input_curr (without protection)", curr)
    series("input_curr + TMR", curr_tmr)

    ax = plt.gca()
    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel(f"mismatch count vs baseline (win={WIN})")
    plt.title("input_curr: mismatch vs baseline (before vs after TMR)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()

# Wykres: amplituda cmd (bez ochrony) vs cmd+SRL + znaczniki flipów
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

    # Pionowe linie flipów
    ymin, ymax = plt.ylim()
    for f in (flips_cmd + flips_srl):
        plt.vlines(f.seq, ymin, ymax, linewidth=0.4, alpha=0.12)

    ax.xaxis.set_major_locator(MultipleLocator(2000))

    plt.xlabel("seq")
    plt.ylabel("A(seq)=max(|mx|,|my|,|mz|)")
    plt.title("output_cmd: control amplitude (before vs after SRL)")
    plt.grid(True, alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(out_png, dpi=160)
    plt.close()

# Policz mismatche względem baseline w oknach WIN
def mismatch_count_vs_baseline(baseline, run, win):
    bins = {}
    for seq, a in run.items():
        # Pomijamy seq=0 i próbki, których nie ma w baseline
        if seq == 0 or seq not in baseline:
            continue
        b = baseline[seq]
        mismatch = int((a.mx != b.mx) or (a.my != b.my) or (a.mz != b.mz))
        end = ((seq + win - 1) // win) * win
        bins[end] = bins.get(end, 0) + mismatch

    xs = sorted(bins.keys())
    ys = [bins[x] for x in xs]
    return xs, ys

# Wykres: mismatch count vs baseline dla kilku scenariuszy
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

# Policz metryki kosztu ochrony
def compute_cost_metrics(name: str, cost: Cost, n_samples: int, n_seu: int):
    # Ile wywołań TMR przypada na próbkę
    tmr_rate = cost.tmr_calls / n_samples if n_samples else 0.0
    # Ile clampów SRL wypada średnio na jedno wywołanie SRL
    if cost.srl_calls > 0:
        srl_clamps_per_call = cost.srl_clamps / cost.srl_calls
    else:
        srl_clamps_per_call = 0.0
    # Ile clampów SRL wypada na jeden przypadek SEU
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

# Zapisz tabelkę CSV z metrykami kosztu ochrony
def save_cost_table(metrics: list[dict], out_csv: Path):
    with out_csv.open("w") as f:
        f.write("name,protect_mode,N_samples,N_seu,tmr_calls,srl_calls,srl_clamps,tmr_rate,srl_clamps_per_call,srl_clamps_per_seu\n")
        for m in metrics:
            f.write(f"{m['name']},{m['protect_mode']},{m['n_samples']},{m['n_seu']},{m['tmr_calls']},{m['srl_calls']},{m['srl_clamps']},{m['tmr_rate']:.6f},{m['srl_clamps_per_call']:.6f},{m['srl_clamps_per_seu']:.6f}\n")

# Liczba próbek w przebiegu (max seq)
def n_samples_from_run(run: dict[int, Actuator]) -> int:
    return max(run.keys()) if run else 0

# Wczytaj logi, wygeneruj wykresy i tabelę kosztów
def main():
    # Utwórz katalog na wyjście
    OUTDIR.mkdir(exist_ok=True)

    # Wczytaj przebiegi: baseline oraz SEU w różnych miejscach
    baseline = parse_act_samples(LOGDIR / "seu_none.log")
    prev = parse_act_samples(LOGDIR / "seu_mode0.log")
    curr = parse_act_samples(LOGDIR / "seu_mode1.log")
    cmd = parse_act_samples(LOGDIR / "seu_mode2.log")

    # Wczytaj przebiegi z ochroną SIFT (TMR dla curr, SRL dla cmd)
    curr_tmr = parse_act_samples(LOGDIR / "seu_mode1_sift.log")
    cmd_srl = parse_act_samples(LOGDIR / "seu_mode2_sift.log")

    # Wczytaj logi z flipami z GDB
    flips_prev = parse_gdb_flips(LOGDIR / "gdb_mode0.log")
    flips_curr = parse_gdb_flips(LOGDIR / "gdb_mode1.log")
    flips_cmd = parse_gdb_flips(LOGDIR / "gdb_mode2.log")
    flips_curr_tmr = parse_gdb_flips(LOGDIR / "gdb_mode1_sift.log")
    flips_cmd_srl = parse_gdb_flips(LOGDIR / "gdb_mode2_sift.log")

    # Zestaw przebiegów przed ochroną do wspólnych wykresów
    pre_runs = {
        "baseline (no SEU)": baseline,
        "SEU in input_prev": prev,
        "SEU in input_curr": curr,
        "SEU in output_cmd": cmd,
    }

    # Flipy do wykresu amplitudy przed ochroną
    pre_flips = {
        "input_prev": flips_prev,
        "input_curr": flips_curr,
        "cmd": flips_cmd,
    }

    # Wykresy: amplituda, saturacje i mismatch vs baseline
    plot_pre_amplitude_all(pre_runs, pre_flips, OUTDIR / "pre_amplitude_all.png")
    plot_saturation_rate(pre_runs, OUTDIR / "pre_sat_rate_all.png")
    plot_mismatch_count(
        baseline,
        {
            "SEU in input_prev": prev,
            "SEU in input_curr": curr,
            "SEU in output_cmd": cmd,
        },
        OUTDIR / "mismatch_count_vs_baseline.png",
    )

    # Wykresy porównawcze: efekt TMR i efekt SRL
    plot_curr_window_compare(curr, curr_tmr, OUTDIR / "curr_sat_rate_compare.png")
    plot_cmd_amplitude_compare(cmd, cmd_srl, flips_cmd, flips_cmd_srl, OUTDIR / "cmd_amplitude_compare.png")
    plot_curr_mismatch_compare(baseline, curr, curr_tmr, OUTDIR / "curr_mismatch_compare.png")

    # Scenariusze do tabeli kosztu
    scenarios = [
        ("none", baseline, LOGDIR / "seu_none.log", []),
        ("input_prev", prev, LOGDIR / "seu_mode0.log", flips_prev),
        ("input_curr", curr, LOGDIR / "seu_mode1.log", flips_curr),
        ("cmd", cmd, LOGDIR / "seu_mode2.log", flips_cmd),
        ("input_curr_tmr", curr_tmr, LOGDIR / "seu_mode1_sift.log", flips_curr_tmr),
        ("cmd_srl", cmd_srl, LOGDIR / "seu_mode2_sift.log", flips_cmd_srl),
    ]

    # Policz metryki kosztu dla scenariuszy, które mają wpis [COST]
    cost_metrics = []
    for name, run, cost_log, flips in scenarios:
        c = parse_cost(cost_log)
        if not c:
            continue
        n_samples = n_samples_from_run(run)
        n_seu = len(flips)
        cost_metrics.append(compute_cost_metrics(name, c, n_samples, n_seu))

    save_cost_table(cost_metrics, OUTDIR / "cost_metrics.csv")

if __name__ == "__main__":
    main()
