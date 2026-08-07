CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size
QEMU := qemu-system-arm

TARGET  = sift_exp

SRCS    = main.c startup.c FreeRTOS/Source/tasks.c FreeRTOS/Source/queue.c FreeRTOS/Source/list.c FreeRTOS/Source/portable/GCC/ARM_CM3/port.c FreeRTOS/Source/portable/MemMang/heap_4.c utils.c

OBJS    = $(SRCS:.c=.o)

CFLAGS  = -mcpu=cortex-m3 -mthumb -O0 -g -ffreestanding -nostdlib -I. -IFreeRTOS/Source/include -IFreeRTOS/Source/portable/GCC/ARM_CM3 -Wall -Wextra -DEXPERIMENT_$(EXP)

LDFLAGS = -T linker.ld -nostdlib

LOGS_DIR = logs

all: $(TARGET).elf $(TARGET).bin

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@ > $(TARGET).size

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf *.bin

run:
	$(QEMU)	-M lm3s6965evb -kernel $(TARGET).bin -nographic

EXPERIMENTS := BASELINE SEU_ONLY QUEUE_PROTECT SEQ_TMR SENSOR_CRC CMD_CLAMP

experiments:
	mkdir -p $(LOGS_DIR)
	@for EXP in $(EXPERIMENTS); do \
		echo "Running $$EXP"; \
		$(MAKE) clean; \
		$(MAKE) EXP=$$EXP; \
		cp $(TARGET).size $(LOGS_DIR)/$$EXP.size; \
		$(QEMU) \
			-M lm3s6965evb \
			-kernel $(TARGET).bin \
			-nographic \
			| tee $(LOGS_DIR)/$$EXP.log; \
	done


baseline:
	$(MAKE) clean
	$(MAKE) EXP=BASELINE
	$(MAKE) run

seu_only:
	$(MAKE) clean
	$(MAKE) EXP=SEU_ONLY
	$(MAKE) run

queue_protect:
	$(MAKE) clean
	$(MAKE) EXP=QUEUE_PROTECT
	$(MAKE) run

seq_tmr:
	$(MAKE) clean
	$(MAKE) EXP=SEQ_TMR
	$(MAKE) run
	
sensor_crc:
	$(MAKE) clean
	$(MAKE) EXP=SENSOR_CRC
	$(MAKE) run


cmd_clamp:
	$(MAKE) clean
	$(MAKE) EXP=CMD_CLAMP
	$(MAKE) run

full:
	$(MAKE) clean
	$(MAKE) EXP=FULL
	$(MAKE) run

# SEU w całą sekcję .data i .bss
data_bss:
	$(MAKE) clean
	$(MAKE) EXP=DATA_BSS
	$(MAKE) run


.PHONY: all clean baseline seu_only seq_tmr cmd_clamp sensor_crc queue_protect full data_bss