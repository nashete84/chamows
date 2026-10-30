# Chamows: build the kernel (build/chamows.bin) and the apps.
#   make          build everything
#   make run      boot it in QEMU
#   make web      put the kernel next to the browser page (run tools/fetch-v86.sh once first)

CC      ?= gcc
BUILD   := build
CFLAGS  := -m32 -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-tree-loop-distribute-patterns -mno-sse -mno-mmx -mno-80387 \
           -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-parameter
KFLAGS  := $(CFLAGS) -fno-pic -fno-pie -Ikernel -I$(BUILD)
AFLAGS  := $(CFLAGS) -fPIE -fvisibility=hidden -Isdk

KSRC    := $(wildcard kernel/*.c)
KOBJ    := $(patsubst kernel/%.c,$(BUILD)/k/%.o,$(KSRC)) $(BUILD)/k/boot.o $(BUILD)/k/isr.o $(BUILD)/k/repo.o
APPS    := calc notepad paint clock snake
APPELF  := $(foreach a,$(APPS),$(BUILD)/apps/$(a).elf)

all: $(BUILD)/chamows.bin

$(BUILD)/assets.h: tools/genassets.py
	@mkdir -p $(BUILD)
	python3 tools/genassets.py $(BUILD)

$(BUILD)/k/%.o: kernel/%.c kernel/*.h sdk/chamows.h $(BUILD)/assets.h
	@mkdir -p $(dir $@)
	$(CC) $(KFLAGS) -c $< -o $@

$(BUILD)/k/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	nasm -f elf32 $< -o $@

$(BUILD)/k/repo.o: kernel/repo.asm $(BUILD)/repo.bin

$(BUILD)/apps/%.elf: apps/%/main.c sdk/chamows.h sdk/cwlib.c
	@mkdir -p $(dir $@)
	$(CC) $(AFLAGS) -c $< -o $(BUILD)/apps/$*.o
	$(CC) $(AFLAGS) -c sdk/cwlib.c -o $(BUILD)/apps/$*-cwlib.o
	ld -m elf_i386 -pie --no-dynamic-linker -z norelro -z noexecstack --hash-style=sysv -e app_main \
	   $(BUILD)/apps/$*.o $(BUILD)/apps/$*-cwlib.o -o $@
	strip $@

$(BUILD)/repo.bin: tools/mkrepo.py $(APPELF) apps/*/manifest.ini $(BUILD)/assets.h
	python3 tools/mkrepo.py $@ $(BUILD)/icons $(foreach a,$(APPS),apps/$(a):$(BUILD)/apps/$(a).elf)

$(BUILD)/chamows.elf: $(KOBJ) kernel/linker.ld
	ld -m elf_i386 -T kernel/linker.ld -o $@ $(KOBJ)

$(BUILD)/chamows.bin: $(BUILD)/chamows.elf
	objcopy -O binary $< $@
	@echo "Listo: $@ ($$(du -k $@ | cut -f1) KB)"

run: $(BUILD)/chamows.bin
	qemu-system-i386 -kernel $< -vga std -m 128 -serial stdio

web: $(BUILD)/chamows.bin
	base64 -w0 $< > web/chamows.b64.txt
	@echo "Abre la página con: python3 -m http.server -d web 8000"

# Boots the kernel in QEMU without a screen and waits for the desktop.
test: $(BUILD)/chamows.bin
	timeout 30 qemu-system-i386 -kernel $< -vga std -m 128 -display none -serial file:$(BUILD)/serial.log & \
	for i in $$(seq 1 25); do sleep 1; grep -q "escritorio listo" $(BUILD)/serial.log 2>/dev/null && break; done; \
	kill %1 2>/dev/null; cat $(BUILD)/serial.log; grep -q "escritorio listo" $(BUILD)/serial.log

clean:
	rm -rf $(BUILD)

.PHONY: all run web test clean
