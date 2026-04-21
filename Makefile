CC = arm-none-eabi-gcc
#CC GDB = gdb-multiarch
OBJCOPY = arm-none-eabi-objcopy
SIZE = arm-none-eabi-size

TARGET  = app
SRCS    = main.c startup.c
OBJS    = $(SRCS:.c=.o)

LDSCRIPT = linker.ld

CFLAGS  = -mcpu=cortex-m3 -mthumb -O0 -g \
          -ffreestanding -nostdlib \
          -Wall -Wextra

LDFLAGS = -T $(LDSCRIPT) -nostdlib

SEU_ENABLE ?= 0
CFLAGS += -DSEU_ENABLE=$(SEU_ENABLE)

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

run-no-seu:
	$(MAKE) clean
	$(MAKE) SEU_ENABLE=0

run-seu:
	$(MAKE) clean
	$(MAKE) SEU_ENABLE=1
