TARGET   := firmware
CC       := arm-none-eabi-gcc
QEMU     := qemu-system-arm
GDB      := gdb-multiarch

FREERTOS := FreeRTOS/Source
PORTDIR  := $(FREERTOS)/portable/GCC/ARM_CM3
HEAPDIR  := $(FREERTOS)/portable/MemMang

PROTECT_MODE ?= 0
SEU_MODE     ?= 0

CFLAGS   := -mcpu=cortex-m3 -mthumb -O0 -g3 -ffreestanding \
            -I. -I$(FREERTOS)/include -I$(PORTDIR) \
            -DPROTECT_MODE=$(PROTECT_MODE)

LDFLAGS  := -T linker.ld --specs=nosys.specs -nostartfiles
LDLIBS   := -lc -lgcc -lnosys

SRCS     := startup.c main.c \
            $(FREERTOS)/tasks.c $(FREERTOS)/queue.c $(FREERTOS)/list.c \
            $(PORTDIR)/port.c $(HEAPDIR)/heap_4.c
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
	$(GDB) $(TARGET).elf -ex 'set $$SEU_MODE=$(SEU_MODE)' -x seu_injector.gdb

clean:
	rm -f $(OBJS) $(TARGET).elf
