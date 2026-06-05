CC = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size
QEMU := qemu-system-arm

TARGET  = app
SRCS    = main.c startup.c $(FREERTOS)/tasks.c $(FREERTOS)/queue.c $(FREERTOS)/list.c $(FREERTOS)/portable/GCC/ARM_CM3/port.c $(FREERTOS)/portable/MemMang/heap_4.c utils.c
OBJS    = $(SRCS:.c=.o)

FREERTOS := FreeRTOS/Source

CFLAGS  = -mcpu=cortex-m3 -mthumb -O0 -g -ffreestanding -nostdlib -I. -I$(FREERTOS)/include -I$(FREERTOS)/portable/GCC/ARM_CM3 -Wall -Wextra -DEXPERIMENT_$(EXP)

LDFLAGS = -T linker.ld -nostdlib

LOGS_DIR = logs

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

seq_tmr:
	$(MAKE) clean
	$(MAKE) EXP=SEQ_TMR
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic
	
sensor_crc:
	$(MAKE) clean
	$(MAKE) EXP=SENSOR_CRC
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic


cmd_clamp:
	$(MAKE) clean
	$(MAKE) EXP=CMD_CLAMP
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

full:
	$(MAKE) clean
	$(MAKE) EXP=FULL
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic

# SEU w całą sekcję .data i .bss
data_bss:
	$(MAKE) clean
	$(MAKE) EXP=DATA_BSS
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).bin -nographic


.PHONY: all clean baseline seu_only seq_tmr cmd_clamp sensor_crc queue_protect full data_bss