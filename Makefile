CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size
QEMU := qemu-system-arm
NM = arm-none-eabi-nm

TARGET = sift_exp

SRCS = main.c startup.c FreeRTOS/Source/tasks.c FreeRTOS/Source/queue.c FreeRTOS/Source/list.c FreeRTOS/Source/portable/GCC/ARM_CM3/port.c FreeRTOS/Source/portable/MemMang/heap_4.c utils.c

OBJS = $(SRCS:.c=.o)

SEU_SEED ?= 0x12345678

MAX_SEQ ?= 100

CFLAGS  = -mcpu=cortex-m3 -mthumb -O0 -g -ffreestanding -nostdlib -I. -IFreeRTOS/Source/include -IFreeRTOS/Source/portable/GCC/ARM_CM3 -Wall -Wextra -DEXPERIMENT_$(EXP) -DSEU_SEED=$(SEU_SEED) -DMAX_SEQ=$(MAX_SEQ)

LDFLAGS = -T linker.ld -nostdlib

LOGS_DIR = logs

all: $(TARGET).elf $(TARGET).bin

$(TARGET).elf: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@ > $(TARGET).size
	$(NM) -n $@ | grep -E '_sseu|_eseu' > $(TARGET).seu_size

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf *.bin *.size *.seu_size *.log

run:
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).elf -nographic -d in_asm,exec,nochain -D exec.log

EXPERIMENTS := QUEUE_PROTECT SEQ_TMR CMD_CLAMP SENSOR_CRC FULL

SEEDS := 0x12345678 0x24688642 0x11111111 0x76893011 0x3579753 0x67676767 0x77761234 0x99763333 0x87654321 0x09322003

experiments:
	@mkdir -p $(LOGS_DIR)
	@echo ""
	@echo "Running BASELINE"
	@echo ""
	@$(MAKE) clean > /dev/null 2>&1
	@$(MAKE) EXP=BASELINE MAX_SEQ=$(MAX_SEQ) > /dev/null 2>&1
	@cp $(TARGET).size $(LOGS_DIR)/BASELINE.size
	@cp $(TARGET).seu_size $(LOGS_DIR)/BASELINE.seu_size || true
	@$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic > $(LOGS_DIR)/BASELINE.log 2>&1 || true
	@$(QEMU) -M lm3s6965evb -kernel $(TARGET).elf -nographic -d in_asm,exec,nochain -D exec.log > /dev/null 2>&1 || true
	@python3 instruction_counter.py exec.log BASELINE $(MAX_SEQ) > $(LOGS_DIR)/BASELINE.instr || true
	@i=1; \
	for SEED in $(SEEDS); do \
		SERIES=$$(printf "exp_%02d" $$i); \
		echo ""; \
		echo "Running $$SERIES with seed=$$SEED"; \
		echo ""; \
		mkdir -p $(LOGS_DIR)/$$SERIES; \
		for EXP in $(EXPERIMENTS); do \
			echo ""; \
			echo "Running $$EXP with seed=$$SEED"; \
			echo ""; \
			$(MAKE) clean > /dev/null 2>&1; \
			$(MAKE) EXP=$$EXP SEU_SEED=$$SEED MAX_SEQ=$(MAX_SEQ) > /dev/null 2>&1; \
			cp $(TARGET).size $(LOGS_DIR)/$$SERIES/$$EXP.size; \
			cp $(TARGET).seu_size $(LOGS_DIR)/$$SERIES/$$EXP.seu_size || true; \
			$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic > $(LOGS_DIR)/$$SERIES/$$EXP.log 2>&1 || true; \
			$(QEMU) -M lm3s6965evb -kernel $(TARGET).elf -nographic -d in_asm,exec,nochain -D exec.log > /dev/null 2>&1 || true; \
			python3 instruction_counter.py exec.log $$EXP $(MAX_SEQ) > $(LOGS_DIR)/$$SERIES/$$EXP.instr || true; \
		done; \
		i=$$((i + 1)); \
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

cmd_rate_limit:
	$(MAKE) clean
	$(MAKE) EXP=CMD_RATE_LIMIT
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


.PHONY: all clean baseline seu_only seq_tmr cmd_clamp sensor_crc queue_protect cmd_rate_limit full data_bss experiments