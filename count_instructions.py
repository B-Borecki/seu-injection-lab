import re
import sys
from collections import defaultdict


if len(sys.argv) != 4:
    print(f"Usage: {sys.argv[0]} exec.log program.elf samples")
    sys.exit(1)

log_file = sys.argv[1]
elf_file = sys.argv[2]
samples = int(sys.argv[3])


# ============================================================
# 1. Parsowanie IN
#
# address -> (liczba instrukcji, funkcja)
# ============================================================

instruction_re = re.compile(
    r"^0x([0-9a-fA-F]+):\s+"
)

in_re = re.compile(
    r"^IN:\s*(.*?)\s*$"
)

tb_info = {}

current_address = None
current_count = 0
current_function = "OTHER"
in_block = False


def save_tb():
    if current_address is not None:
        tb_info[current_address] = (
            current_count,
            current_function
        )


with open(log_file, "r", errors="ignore") as f:

    for line in f:

        line = line.rstrip()

        m = in_re.match(line)

        if m:

            save_tb()

            current_function = m.group(1).strip()

            if not current_function:
                current_function = "OTHER"

            current_address = None
            current_count = 0
            in_block = True

            continue

        if in_block:

            m = instruction_re.match(line)

            if m:

                address = int(m.group(1), 16)

                if current_address is None:
                    current_address = address

                current_count += 1

            elif line.startswith("Trace "):

                in_block = False


save_tb()


# ============================================================
# 2. Parsowanie Trace
#
# address -> liczba wykonań
# ============================================================

trace_re = re.compile(
    r"^Trace\s+\d+:\s+.*?\[.*?/([0-9a-fA-F]+)/"
)

trace_count = 0
trace_executions = defaultdict(int)

with open(log_file, "r", errors="ignore") as f:

    for line in f:

        m = trace_re.match(line)

        if not m:
            continue

        address = int(m.group(1), 16)

        trace_count += 1
        trace_executions[address] += 1


# ============================================================
# 3. Liczenie
# ============================================================

total_instructions = 0

groups = {
    "task_sensor": 0,
    "task_controller": 0,
    "task_actuator": 0,
    "OTHER": 0,
}

unknown_tbs = 0


for address, executions in trace_executions.items():

    info = tb_info.get(address)

    if info is None:
        unknown_tbs += executions
        continue

    instruction_count, function = info

    dynamic = instruction_count * executions

    total_instructions += dynamic

    if function == "task_sensor":
        groups["task_sensor"] += dynamic

    elif function == "task_controller":
        groups["task_controller"] += dynamic

    elif function == "task_actuator":
        groups["task_actuator"] += dynamic

    else:
        groups["OTHER"] += dynamic


# ============================================================
# 4. Wynik
# ============================================================

adcs_instructions = (
    groups["task_sensor"]
    + groups["task_controller"]
    + groups["task_actuator"]
)

print()
print("Dynamic instruction count")
print("-------------------------")

print(f"Samples          : {samples}")
print(f"Unique TBs       : {len(tb_info)}")
print(f"Trace executions : {trace_count}")
print(f"Total            : {total_instructions}")

print()
print("By task")
print("-------------------------")

print(f"task_sensor      : {groups['task_sensor']}")
print(f"task_controller  : {groups['task_controller']}")
print(f"task_actuator    : {groups['task_actuator']}")
print(f"ADCS total       : {adcs_instructions}")
print(f"OTHER            : {groups['OTHER']}")

print()
print("Per sample")
print("-------------------------")

print(f"Total / sample   : {total_instructions / samples:.2f}")
print(f"ADCS / sample    : {adcs_instructions / samples:.2f}")

print()
print(f"Unknown TB exec. : {unknown_tbs}")