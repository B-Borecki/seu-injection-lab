import re
from pathlib import Path
from statistics import mean, stdev


LOGS_DIR = Path("logs")
EXPERIMENTS = [
    "QUEUE_PROTECT",
    "SEQ_TMR",
    "CMD_CLAMP",
    "SENSOR_CRC",
    "FULL"]


def stats_from_log(path):
    text = path.read_text()
    fields = ["samples", "SEUs", "good samples", "errors", "samples omitted", "propagations", "detected disturbances", "tmr corrections", "dmr corrections", "crc detections", "clamp activations"]

    stats = {}
    for field in fields:
        m = re.search(rf"^{re.escape(field)}=(\d+)", text, re.MULTILINE)
        stats[field] = int(m.group(1))
    m = re.search(r"^max error=(0x[0-9A-Fa-f]+)", text, re.MULTILINE)
    stats["max_error"] = int(m.group(1), 16)
    return stats


def instr_from_log(path):
    text = path.read_text()
    patterns = {
        "total_instructions": r"Total\s*:\s*([0-9]+)",
        "sift_instructions": r"SIFT\s*:\s*([0-9]+)",
        "total_per_sample": r"Total/sample\s*:\s*([0-9.]+)",
        "sift_per_sample": r"SIFT/sample\s*:\s*([0-9.]+)",
        "sift_percent": r"SIFT/Total\s*:\s*([0-9.]+)%",
        "unique_tbs": r"Unique TBs\s*:\s*([0-9]+)",
        "trace_executions": r"Trace executions\s*:\s*([0-9]+)",
        "unknown_tb_executions": r"Unknown TB executions\s*:\s*([0-9]+)"}
    stats = {}
    for key, pattern in patterns.items():
        m = re.search(pattern, text)
        value = m.group(1)
        stats[key] = float(value) if "." in value else int(value)
    return stats


def size_from_log(path):
    m = re.search(r"^\s*([0-9]+)\s+([0-9]+)\s+([0-9]+)\s+[0-9]+\s+[0-9A-Fa-f]+", path.read_text(), re.MULTILINE)
    text, data, bss = map(int, m.groups())
    return {
        "flash": text + data,
        "ram": data + bss}


def seu_size_from_log(path):
    text = path.read_text()
    start = re.search(r"^\s*([0-9A-Fa-f]+)\s+\S+\s+_sseu", text, re.MULTILINE)
    end = re.search(r"^\s*([0-9A-Fa-f]+)\s+\S+\s+_eseu", text, re.MULTILINE)
    seu_size = int(end.group(1), 16) - int(start.group(1), 16)
    return {"seu_size": seu_size}


def collect_all_stats():
    series = sorted(p for p in LOGS_DIR.iterdir() if p.is_dir() and p.name.startswith("exp_"))
    stats = {experiment: [] for experiment in EXPERIMENTS}
    for experiment in EXPERIMENTS:
        for exp in series:
            paths = {
                "log": exp/f"{experiment}.log",
                "instr": exp/f"{experiment}.instr",
                "size": exp/f"{experiment}.size",
                "seu": exp/f"{experiment}.seu_size"}
            data = stats_from_log(paths["log"])
            data.update(instr_from_log(paths["instr"]))
            data.update(size_from_log(paths["size"]))
            data.update(seu_size_from_log(paths["seu"]))
            stats[experiment].append(data)
    return stats


def get_baseline_instr():
    path = LOGS_DIR/"BASELINE.instr"
    return instr_from_log(path)


def print_main_table(all_stats):
    print("\nAVERAGE RESULTS FOR EACH EXPERIMENT")
    print("-" * 110)
    print(
        f"{'Experiment':<18}"
        f"{'SEU injections':>14}"
        f"{'Errors':>14}"
        f"{'Propagations':>18}"
        f"{'Detected disturbances':>26}"
        f"{'Max error':>22}")
    print("-" * 110)
    for experiment in EXPERIMENTS:
        stats = all_stats[experiment]
        print(
            f"{experiment:<18}"
            f"{mean(x['SEUs'] for x in stats):>14.2f}"
            f"{mean(x['errors'] for x in stats):>14.2f}"
            f"{mean(x['propagations'] for x in stats):>18.2f}"
            f"{mean(x['detected disturbances'] for x in stats):>26.2f}"
            f"{mean(x['max_error'] for x in stats):>22.2f}")
    print("-" * 110)


def print_variability_statistics(all_stats):
    print("\nVARIABILITY OF EXPERIMENT RESULTS")
    print("-" * 210)
    print(
        f"{'Experiment':<18}"
        f"{'Errors Mean':>14}"
        f"{'Errors Std.Dev.':>18}"
        f"{'Errors Min':>14}"
        f"{'Errors Max':>14}"
        f"{'Propagations Mean':>22}"
        f"{'Propagations Std.Dev.':>24}"
        f"{'Propagations Min':>20}"
        f"{'Propagations Max':>20}"
        f"{'Max error Mean':>22}"
        f"{'Max error Std.Dev.':>24}")
    print("-" * 210)
    for experiment in EXPERIMENTS:
        stats = all_stats[experiment]
        errors = [x["errors"] for x in stats]
        propagations = [x["propagations"] for x in stats]
        max_errors = [x["max_error"] for x in stats]
        print(
            f"{experiment:<18}"
            f"{mean(errors):>14.2f}"
            f"{stdev(errors):>18.2f}"
            f"{min(errors):>14.2f}"
            f"{max(errors):>14.2f}"
            f"{mean(propagations):>22.2f}"
            f"{stdev(propagations):>24.2f}"
            f"{min(propagations):>20.2f}"
            f"{max(propagations):>20.2f}"
            f"{mean(max_errors):>22.2f}"
            f"{stdev(max_errors):>24.2f}")
    print("-" * 210)


def print_detection_statistics(all_stats):
    print("\nPROTECTION MECHANISM ACTIVITY")
    print("-" * 140)
    print(
        f"{'Experiment':<18}"
        f"{'Detected disturbances':>22}"
        f"{'TMR corrections':>20}"
        f"{'DMR+CRC corrections':>25}"
        f"{'CRC detections':>20}"
        f"{'Clamp activations':>22}")
    print("-" * 140)
    for experiment in EXPERIMENTS:
        stats = all_stats[experiment]
        print(
            f"{experiment:<18}"
            f"{mean(x['detected disturbances'] for x in stats):>22.2f}"
            f"{mean(x['tmr corrections'] for x in stats):>20.2f}"
            f"{mean(x['dmr corrections'] for x in stats):>25.2f}"
            f"{mean(x['crc detections'] for x in stats):>20.2f}"
            f"{mean(x['clamp activations'] for x in stats):>22.2f}")
    print("-" * 140)


def print_size_table(all_stats):
    print("\nPROGRAM MEMORY AND RAM USAGE")
    print("-" * 105)
    print(
        f"{'Experiment':<18}"
        f"{'Flash [B]':>22}"
        f"{'RAM [B]':>20}"
        f"{'.seu_section [B]':>24}")
    print("-" * 105)
    size_path = LOGS_DIR / "BASELINE.size"
    size = size_from_log(size_path)
    seu = seu_size_from_log(LOGS_DIR/"BASELINE.seu_size")
    print(
        f"{'BASELINE':<18}"
        f"{size['flash']:>22.2f}"
        f"{size['ram']:>20.2f}"
        f"{seu['seu_size']:>24.2f}")
    for experiment in EXPERIMENTS:
        stats = all_stats[experiment]
        print(
            f"{experiment:<18}"
            f"{mean(x['flash'] for x in stats):>22.2f}"
            f"{mean(x['ram'] for x in stats):>20.2f}"
            f"{mean(x['seu_size'] for x in stats):>24.2f}")
    print("-" * 105)


def print_instruction_table(all_stats):
    print("\nDYNAMIC INSTRUCTION COUNT")
    print("-" * 125)
    print(
        f"{'Experiment':<18}"
        f"{'Total/sample':>18}"
        f"{'SIFT/sample':>18}"
        f"{'SIFT/Total':>16}"
        f"{'Total instructions':>22}"
        f"{'Overhead vs BASELINE':>25}")
    print("-" * 125)
    baseline = get_baseline_instr()
    baseline_total = baseline["total_per_sample"]
    print(
        f"{'BASELINE':<18}"
        f"{baseline['total_per_sample']:>18.2f}"
        f"{baseline['sift_per_sample']:>18.2f}"
        f"{baseline['sift_percent']:>15.2f}%"
        f"{baseline['total_instructions']:>22.2f}"
        f"{'0.00%':>25}")
    for experiment in EXPERIMENTS:
        stats = all_stats[experiment]
        total = mean(x["total_per_sample"] for x in stats)
        overhead = (total - baseline_total) / baseline_total * 100
        print(
            f"{experiment:<18}"
            f"{total:>18.2f}"
            f"{mean(x["sift_per_sample"] for x in stats):>18.2f}"
            f"{mean(x["sift_percent"] for x in stats):>15.2f}%"
            f"{mean(x["total_instructions"] for x in stats):>22.2f}"
            f"{overhead:>24.2f}%")
    print("-" * 125)


def main():
    stats = collect_all_stats()
    print_main_table(stats)
    print_variability_statistics(stats)
    print_detection_statistics(stats)
    print_size_table(stats)
    print_instruction_table(stats)


if __name__ == "__main__":
    main()