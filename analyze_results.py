import re
import sys
from pathlib import Path
from statistics import mean, stdev


LOGS_DIR = Path("logs")

EXPERIMENTS = ["QUEUE_PROTECT", "SEQ_TMR", "CMD_CLAMP", "SENSOR_CRC", "FULL"]


def stats_from_log(path):
    text = path.read_text()
    fields = [
        "samples",
        "SEUs",
        "good samples",
        "errors",
        "samples omitted",
        "propagations",
        "detected_errors",
        "tmr_corrections",
        "dmr_corrections",
        "crc detections",
        "clamp_activations",
        "rate limit activations"]
    stats = {}
    for field in fields:
        match = re.search(rf"{field}=(\d+)", text)
        if match:
            stats[field] = int(match.group(1))
        else:
            stats[field] = 0
    match = re.search(r"max_error=(0x[0-9A-Fa-f]+)", text)
    if match:
        stats["max_error"] = int(match.group(1), 16)
    else:
        stats["max_error"] = 0

    stats["detection_rate"] = stats["detected_errors"] / stats["SEUs"] * 100.0
    return stats


def collect_all_stats():
    stats = {}
    series = sorted(p for p in LOGS_DIR.iterdir())
    for experiment in EXPERIMENTS:
        stats[experiment] = []
        for exp in series:
            log_path = exp/f"{experiment}.log"
            file_stats = stats_from_log(log_path)
            file_stats["series"] = exp.name
            stats[experiment].append(file_stats)
    return stats


def print_main_table(all_stats):
    print()
    print("Average number of injected SEUs per run: 1835 (10 independent runs)")
    print()
    print("AVERAGE RESULTS FOR EACH EXPERIMENT (each value is an average)")
    print("-" * 100)
    header = (
        f"{'Experiment':<18}"
        f"{'Errors':>14}"
        f"{'Propagations':>18}"
        f"{'Detected disturbances':>26}"
        f"{'Max error':>14}")
    
    print(header)
    print("-" * 100)
    for experiment in EXPERIMENTS:
        stats = all_stats[experiment]
        print(
            f"{experiment:<18}"
            f"{mean(x['errors'] for x in stats):>14.2f}"
            f"{mean(x['propagations'] for x in stats):>15.2f}"
            f"{mean(x['detected_errors'] for x in stats):>22.2f}"
            f"{mean(x['max_error'] for x in stats):>22.2f}")
    print("-" * 100)
    print()
    


def print_variability_statistics(all_stats):
    print()
    print("VARIABILITY OF EXPERIMENT RESULTS")
    print("-" * 210)
    header = (
        f"{'Experiment':<18}"
        f"{'Errors Mean':>14}"
        f"{'Errors Std.Dev.':>17}"
        f"{'Errors Min':>14}"
        f"{'Errors Max':>14}"
        f"{'Propagations Mean':>22}"
        f"{'Propagations Std.Dev.':>24}"
        f"{'Propagations Min':>20}"
        f"{'Propagations Max':>20}"
        f"{'Max error Mean':>22}"
        f"{'Max error Std.Dev.':>24}")
    print(header)
    print("-" * 210)
    for experiment in EXPERIMENTS:
        stats = all_stats[experiment]
        errors = [x["errors"] for x in stats]
        propagations = [x["propagations"] for x in stats]
        max_errors = [x["max_error"] for x in stats]
        print(
            f"{experiment:<16}"
            f"{mean(errors):>14.2f}"
            f"{stdev(errors):>14.2f}"
            f"{min(errors):>17.2f}"
            f"{max(errors):>14.2f}"
            f"{mean(propagations):>18.2f}"
            f"{stdev(propagations):>22.2f}"
            f"{min(propagations):>22.2f}"
            f"{max(propagations):>20.2f}"
            f"{mean(max_errors):>26.2f}"
            f"{stdev(max_errors):>22.2f}")
    print("-" * 210)
    print()


def print_detection_statistics(all_stats):
    print()
    print("PROTECTION MECHANISM ACTIVITY")
    print("-" * 140)
    header = (
        f"{'Experiment':<18}"
        f"{'Detected disturbances':>20}"
        f"{'TMR corrections':>20}"
        f"{'DMR+CRC corrections':>25}"
        f"{'CRC detections':>20}"
        f"{'Clamp activations':>22}")
    print(header)
    print("-" * 140)
    for experiment in EXPERIMENTS:
        stats = all_stats[experiment]
        print(
            f"{experiment:<18}"
            f"{mean(x['detected_errors'] for x in stats):>12.2f}"
            f"{mean(x['tmr_corrections'] for x in stats):>24.2f}"
            f"{mean(x['dmr_corrections'] for x in stats):>22.2f}"
            f"{mean(x['crc detections'] for x in stats):>22.2f}"
            f"{mean(x['clamp_activations'] for x in stats):>22.2f}")
    print("-" * 140)
    print()


def print_size_table():
    print()
    print("PROGRAM MEMORY AND RAM USAGE")
    print("-" * 100)
    header = (
        f"{'Experiment':<18}"
        f"{'Flash [B]':>22}"
        f"{'RAM [B]':>20}"
        f"{'.seu_section':>20}")
    print(header)
    print("-" * 100)
    for experiment in EXPERIMENTS:
        size_path = LOGS_DIR/"exp_05"/f"{experiment}.size"
        seu_size_path = LOGS_DIR/"exp_05"/f"{experiment}.seu_size"
        lines = size_path.read_text().strip().splitlines()
        values = lines[1].split()
        text = int(values[0])
        data = int(values[1])
        bss = int(values[2])
        flash = text + data
        ram = data + bss
        seu_lines = seu_size_path.read_text().strip().splitlines()
        addresses = {}
        for line in seu_lines:
            fields = line.split()
            if len(fields) >= 3 and fields[2] in ("_sseu", "_eseu"):
                addresses[fields[2]] = int(fields[0], 16)
            seu_size = addresses["_eseu"] - addresses["_sseu"]
        print(
            f"{experiment:<18}"
            f"{flash:>22}"
            f"{ram:>20}"
            f"{seu_size:>18}")
    print("-" * 100)


def main():
    stats = collect_all_stats()
    print_main_table(stats)
    print_variability_statistics(stats)
    print_detection_statistics(stats)
    print_size_table()


if __name__ == "__main__":
    main()