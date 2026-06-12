# seu-injection-lab

A laboratory project for evaluating software-based fault-tolerance techniques for Single Event Upsets (SEUs) mitigation in embedded systems.

The application simulates a simplified ADCS-inspired control loop running on an ARM Cortex-M3 microcontroller under FreeRTOS. Randomly injected bit flips are used to evaluate the effectiveness of several protection mechanisms, including:
- Triple Modular Redundancy (TMR) for sequence counters,
- CRC protection of sensor samples,
- CRC + DMR protection of queue pointers,
- Command value clamping,
- Combined protection mechanisms.

## Project structure

- `main.c` - application logic, fault injection and protection mechanisms.
- `FreeRTOS/` - FreeRTOS kernel source code.
- `utils.*` - UART support and helper functions.
- `linker.ld` - linker script defining memory regions.

## Experiments

| Build target | Description |
|-------------|-------------|
| `make baseline` | Baseline version without SEU injection. |
| `make seu_only` | SEU fault injection enabled, no protection mechanisms applied. |
| `make queue_protect` | Queue pointer protection using DMR and CRC. |
| `make seq_tmr` | Triple Modular Redundancy (TMR) protection of the sequence counter. |
| `make sensor_crc` | CRC protection of sensor data samples. |
| `make cmd_clamp` | Command clamping to limit actuator control values. |
| `make full` | Full configuration with all protection mechanisms enabled. |

## Building

The project requires:

- `arm-none-eabi-gcc`
- `arm-none-eabi-objcopy`
- `arm-none-eabi-size`
- `make`

## Run
Example:
```bash
make full
