N64_INST ?= /n64_toolchain

PROG_NAME = mulmul_characterize

OBJS = main.o

include $(N64_INST)/include/n64.mk

$(PROG_NAME).elf: $(OBJS)

clean:
	rm -f *.o *.elf *.z64

.PHONY: clean