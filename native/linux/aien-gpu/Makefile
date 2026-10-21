# aien-gpu: the Linux host shim for our own GB10 GPU driver (L332 rung 2). Build only; loading it is a
# coordinator-scheduled window step (docs/GPU_DRIVER.md, evidence/own-gpu-driver/rung2/RUNBOOK.md).
KDIR ?= /lib/modules/$(shell uname -r)/build
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
.PHONY: all clean
