obj-m := hid-maschine-jam.o
KVERSION := $(shell uname -r)
MAKEFILE_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

all:
	$(MAKE) -C /lib/modules/$(KVERSION)/build M=$(MAKEFILE_DIR) modules

clean:
	$(MAKE) -C /lib/modules/$(KVERSION)/build M=$(MAKEFILE_DIR) clean
