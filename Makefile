CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size
QEMU := qemu-system-arm

TARGET  = app
SRCS    = main.c startup.c $(FREERTOS)/tasks.c $(FREERTOS)/queue.c $(FREERTOS)/list.c $(FREERTOS)/portable/GCC/ARM_CM3/port.c $(FREERTOS)/portable/MemMang/heap_4.c utils.c
OBJS    = $(SRCS:.c=.o)

FREERTOS := FreeRTOS/Source

CFLAGS  = -mcpu=cortex-m3 -mthumb -O0 -g \
          -ffreestanding -nostdlib -I. -I$(FREERTOS)/include -I$(FREERTOS)/portable/GCC/ARM_CM3 \
          -Wall -Wextra -DEXPERIMENT_$(EXP)

LDFLAGS = -T linker.ld -nostdlib

all: $(TARGET).elf $(TARGET).bin

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf *.bin

baseline:
	$(MAKE) clean
	$(MAKE) EXP=BASELINE
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

seu_only:
	$(MAKE) clean
	$(MAKE) EXP=SEU_ONLY
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

queue_protect:
	$(MAKE) clean
	$(MAKE) EXP=QUEUE_PROTECT
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

tmr_clamp:
	$(MAKE) clean
	$(MAKE) EXP=SEQ_TMR_AND_CLAMP
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

sensor_crc:
	$(MAKE) clean
	$(MAKE) EXP=SENSOR_CRC
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

full:
	$(MAKE) clean
	$(MAKE) EXP=FULL
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

data_bss:
	$(MAKE) clean
	$(MAKE) EXP=DATA_BSS
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

all_experiments:
	@echo "=== BASELINE ===" > results.txt
	$(MAKE) baseline >> results.txt 2>&1
	@echo "\n=== QUEUE_PROTECT ===" >> results.txt
	$(MAKE) queue_protect >> results.txt 2>&1
	@echo "\n=== SEQ_TMR ===" >> results.txt
	$(MAKE) seq_tmr >> results.txt 2>&1
	@echo "\n=== SENSOR_CRC ===" >> results.txt
	$(MAKE) sensor_crc >> results.txt 2>&1
	@echo "\n=== FULL ===" >> results.txt
	$(MAKE) full >> results.txt 2>&1
	@echo "Wyniki zapisane w results.txt"


.PHONY: all clean baseline seu_only seq_tmr sensor_crc queue_protect full all_experiments