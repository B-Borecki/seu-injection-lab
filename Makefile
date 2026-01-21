TARGET   := firmware
CC       := arm-none-eabi-gcc
GDB      := arm-none-eabi-gdb
QEMU     := qemu-system-arm

FREERTOS := FreeRTOS/Source
PORTDIR  := $(FREERTOS)/portable/GCC/ARM_CM3
HEAPDIR  := $(FREERTOS)/portable/MemMang

CFLAGS   := -mcpu=cortex-m3 -mthumb -O0 -g3 -ffreestanding -I. -I$(FREERTOS)/include -I$(PORTDIR) -DSEFI_MODE=1

LDFLAGS  := -T linker.ld --specs=nosys.specs -nostartfiles
LDLIBS   := -lc -lgcc -lnosys

SRCS     := startup.c main.c $(FREERTOS)/tasks.c $(FREERTOS)/queue.c $(FREERTOS)/list.c	$(PORTDIR)/port.c $(HEAPDIR)/heap_4.c
OBJS     := $(SRCS:.c=.o)

.PHONY: all clean run run-debug gdb

all: $(TARGET).elf

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDLIBS) -o $@

run: $(TARGET).elf
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).elf -nographic

run-debug: $(TARGET).elf
	$(QEMU) -M lm3s6965evb -kernel $(TARGET).elf -nographic -S -s

gdb: $(TARGET).elf
	$(GDB) $(TARGET).elf -x seu_injector.gdb

clean:
	rm -f $(OBJS) $(TARGET).elf
