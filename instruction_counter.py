import re
import sys
from collections import defaultdict

SIFT_GROUPS = {
    "BASELINE": (),

    "QUEUE_PROTECT": {
        "get_protected_queue",
        "crc8",
        "crc8_ptr",
        "get_sensor_samples",
        "get_coil_cmds",
        "init_protected_queue",
        "set_sensor_samples",
        "set_coil_cmds"
    },

    "SEQ_TMR": {
        "get_protected_queue",
        "crc8",
        "crc8_ptr",
        "get_sensor_samples",
        "get_coil_cmds",
        "init_protected_queue",
        "set_sensor_samples",
        "set_coil_cmds",
        "get_seq",
        "inc_seq"
    },

    "CMD_CLAMP": {
        "get_protected_queue",
        "crc8",
        "crc8_ptr",
        "get_sensor_samples",
        "get_coil_cmds",
        "init_protected_queue",
        "set_sensor_samples",
        "set_coil_cmds",
        "get_seq",
        "inc_seq",
        "clamp_cmd"
    },

    "SENSOR_CRC": {
        "get_protected_queue",
        "crc8",
        "crc8_ptr",
        "get_sensor_samples",
        "get_coil_cmds",
        "init_protected_queue",
        "set_sensor_samples",
        "set_coil_cmds",
        "get_seq",
        "inc_seq",
        "clamp_cmd",
        "crc8_sensor"
    },

    "FULL": {
        "get_protected_queue",
        "crc8",
        "crc8_ptr",
        "get_sensor_samples",
        "get_coil_cmds",
        "init_protected_queue",
        "set_sensor_samples",
        "set_coil_cmds",
        "get_seq",
        "inc_seq",
        "clamp_cmd",
        "crc8_sensor"
    }
}


log_file = sys.argv[1]
experiment = sys.argv[2]

instruction_regex = re.compile(r"^0x([0-9a-fA-F]+):")
in_regex = re.compile(r"^IN:\s*(.*?)\s*$")

tb_info = {}
current_address = None
current_count = 0
current_function = "OTHER"
in_block = False
with open(log_file, "r") as f:
    for line in f:
        match = in_regex.match(line)
        if match:
            if current_address is not None:
                tb_info[current_address] = (current_count, current_function)
            current_function = match.group(1) or "OTHER"
            current_address = None
            current_count = 0
            in_block = True
            continue
        if in_block:
            match = instruction_regex.match(line)
            if match:
                address = int(match.group(1), 16)
                if current_address is None:
                    current_address = address
                current_count += 1
            else:
                in_block = False
tb_info[current_address] = (current_count, current_function)


trace_re = re.compile(r"^Trace\s+[0-9]+:\s+.*?\[.*?/([0-9a-fA-F]+)/")
trace_count = 0
trace_executions = defaultdict(int)
with open(log_file, "r") as f:
    for line in f:
        match = trace_re.match(line)
        if match:
            address = int(match.group(1), 16)
            trace_executions[address] += 1
            trace_count += 1


total_instructions = 0
sift_instructions = 0
unknown_tbs = 0
for address, executions in trace_executions.items():
    info = tb_info.get(address)
    if info is None:
        unknown_tbs += executions
        continue
    instruction_count, func_name = info
    dynamic = instruction_count * executions
    if func_name in SIFT_GROUPS[experiment]:
        sift_instructions += dynamic
    total_instructions += dynamic


print("---------------------------")
print("Dynamic instruction count")
print("---------------------------")
print(f"Experiment       : {experiment}")
print(f"Total            : {total_instructions}")
print(f"SIFT             : {sift_instructions}")
samples = 500
print(f"Total/sample     : {total_instructions / samples}")
print(f"SIFT/sample      : {sift_instructions / samples}")
sift_percentage = 100.0 * sift_instructions / total_instructions
print(f"SIFT/Total       : {sift_percentage:.2f}%")
print("---------------------------")
print()
print("Technical checks")
print(f"Unique TBs            : {len(tb_info)}")
print(f"Trace executions      : {trace_count}")
print(f"Unknown TB executions : {unknown_tbs}")