# seu-injection-lab

A laboratory project for implementing and analyzing software-based fault-tolerance mechanisms against Single Event Upset (SEU) errors in embedded systems.

The application implements a simplified ADCS-inspired control loop running on an ARM Cortex-M3 platform under FreeRTOS. Controlled single-bit SEU injections are used to observe the behavior of the application and the implemented protection mechanisms, including:
- Triple Modular Redundancy (TMR) for the sequence counter,
- CRC protection of sensor samples,
- DMR + CRC protection of queue pointers,
- Command value clamping,
- Combined protection mechanisms.

## Project structure

- `main.c` - application logic, SEU injection and protection mechanisms.
- `FreeRTOS/` - FreeRTOS kernel source code.
- `utils.*` - UART support and helper functions.
- `linker.ld` - linker script defining memory regions.
- `instruction_counter.py` - analysis of dynamically executed instructions from QEMU instrumentation logs.
- `analyze_results.py` - analysis and aggregation of experimental results.

## Experiments

| Build target | Description |
|-------------|-------------|
| `make baseline` | Baseline version without SEU injection. |
| `make seu_only` | SEU injection enabled without protection mechanisms. |
| `make queue_protect` | Queue pointer protection using DMR and CRC. |
| `make seq_tmr` | Sequence counter protection using TMR. |
| `make sensor_crc` | CRC-based integrity checking of sensor samples. |
| `make cmd_clamp` | Command value clamping to a defined range. |
| `make full` | Configuration combining all implemented protection mechanisms. |
| `make experiments` | Runs the complete experimental series for all protection configurations and SEU seeds. |

## Running the experiments

The complete experimental procedure is automated using:

```bash
make experiments
```

The target runs the baseline and all protection configurations for ten different SEU injection seeds. For each run, the program is compiled, its memory usage is recorded, and the application is executed in QEMU. The resulting logs and measurement files are stored in the logs/ directory.

For each configuration, QEMU is also executed separately with instruction-level instrumentation enabled. The generated execution log is processed by instruction_counter.py to determine the number of dynamically executed instructions.

After completing the experiments, the collected results can be analyzed using:
```
python3 analyze_results.py
```
The analysis script aggregates the results from all runs and generates statistics for erroneous samples, error propagation, detected disturbances, protection mechanism activity, memory usage, and dynamically executed instructions.

## Building

The project requires:

arm-none-eabi-gcc
arm-none-eabi-objcopy
arm-none-eabi-size
arm-none-eabi-nm
qemu-system-arm
make
python3

## Running a single configuration

Individual configurations can also be built and executed directly. For example:
```
make full
```
or:
```
make queue_protect
make seq_tmr
make sensor_crc
make cmd_clamp
```

The application is executed in QEMU using the `lm3s6965evb` machine model, which provides an emulated ARM Cortex-M3 platform. FreeRTOS is used as the real-time operating system, while SEU errors are modeled as controlled single-bit flips injected into selected program data during execution.
