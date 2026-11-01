# Chamows: build the kernel and the apps for one CPU architecture.
#   make                 64-bit build (build/x86_64/chamows.bin)  -> QEMU, USB
#   make ARCH=i386       32-bit build (build/i386/chamows.bin)    -> browser (v86)
#   make both            both of them
#   make run [ARCH=..]   boot it in QEMU
#   make test [ARCH=..]  boot without a screen and wait for the desktop
#   make web             put the 32-bit kernel next to the browser page

ARCH    ?= x86_64
CC      ?= gcc
BUILD   := build/$(ARCH)
COMMON  := -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
           -fno-tree-loop-distribute-patterns -mno-sse -mno-mmx -mno-80387 \
           -fno-asynchronous-unwind-tables -Wall -Wextra -Wno-unused-parameter

ifeq ($(ARCH),x86_64)
  # no red zone: interrupts run on the same stack as the kernel and the apps
  CFLAGS := -m64 -mno-red-zone $(COMMON)
  NASMF  := elf64
  LDEMU  := elf_x86_64
  QEMU   := qemu-system-x86_64
  # USB controller + USB keyboard, tablet (absolute mouse) and a FAT32 pendrive
  QEMU_USB := -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
              -drive if=none,id=stick,format=raw,file=$(BUILD)/pendrive.img -device usb-storage,bus=xhci.0,drive=stick
else ifeq ($(ARCH),i386)
  CFLAGS := -m32 $(COMMON)
  NASMF  := elf32
  LDEMU  := elf_i386
  QEMU   := qemu-system-i386
  QEMU_USB :=
else
  $(error ARCH tiene que ser x86_64 o i386)
endif

KFLAGS  := $(CFLAGS) -fno-pic -fno-pie -Ikernel -Ikernel/arch/$(ARCH) -I$(BUILD)
AFLAGS  := $(CFLAGS) -fPIE -fvisibility=hidden -Isdk

KSRC    := $(wildcard kernel/*.c) $(wildcard kernel/usb/*.c) $(wildcard kernel/win32/*.c) $(wildcard kernel/net/*.c)
KOBJ    := $(patsubst kernel/%.c,$(BUILD)/k/%.o,$(KSRC)) $(BUILD)/k/boot.o $(BUILD)/k/isr.o $(BUILD)/k/repo.o
APPS    := calc notepad paint clock snake burbujas
APPELF  := $(foreach a,$(APPS),$(BUILD)/apps/$(a).elf)
# Windows programs (.exe) for the Store: only the 64-bit Chamows runs them
WINCC   ?= x86_64-w64-mingw32-gcc
WINAPPS := $(if $(filter x86_64,$(ARCH)),hola esquiva)
WINEXE  := $(foreach a,$(WINAPPS),$(BUILD)/winapps/$(a).exe)

all: $(BUILD)/chamows.bin

both:
	$(MAKE) ARCH=x86_64
	$(MAKE) ARCH=i386

# fonts and icons are the same for both architectures
build/assets.h: tools/genassets.py
	@mkdir -p build
	python3 tools/genassets.py build

$(BUILD)/assets.h: build/assets.h
	@mkdir -p $(BUILD)
	cp $< $@

$(BUILD)/k/%.o: kernel/%.c kernel/*.h kernel/usb/*.h kernel/net/*.h kernel/arch/$(ARCH)/arch.h sdk/chamows.h $(BUILD)/assets.h
	@mkdir -p $(dir $@)
	$(CC) $(KFLAGS) -c $< -o $@

# the Win32 layer runs Windows code, which uses SSE
$(BUILD)/k/win32/%.o: KFLAGS += -msse -msse2 -m80387 -mfpmath=sse
$(BUILD)/k/win32/win32.o: kernel/win32/wfiles.inc kernel/win32/wgui.inc kernel/win32/wsock.inc kernel/win32/wdll.inc kernel/win32/wsync.inc kernel/win32/wseh.inc kernel/win32/wcrt.inc kernel/win32/wmisc.inc kernel/win32/wuser.inc kernel/win32/wfont.inc kernel/win32/wtext.inc kernel/win32/wicon.inc kernel/win32/stb_truetype.h kernel/win32/iconfont.h

$(BUILD)/winapps/%.exe: winapps/%/main.c
	@mkdir -p $(dir $@)
	$(WINCC) -O2 -s $< -o $@ $$(grep -q WinMain $< && echo -mwindows) -lgdi32 -luser32

$(BUILD)/k/%.o: kernel/arch/$(ARCH)/%.asm
	@mkdir -p $(dir $@)
	nasm -f $(NASMF) $< -o $@

$(BUILD)/k/repo.o: kernel/repo.asm $(BUILD)/repo.bin
	@mkdir -p $(dir $@)
	nasm -f $(NASMF) -DREPO_BIN='"$(BUILD)/repo.bin"' $< -o $@

$(BUILD)/apps/%.elf: apps/%/main.c sdk/chamows.h sdk/cwlib.c
	@mkdir -p $(dir $@)
	$(CC) $(AFLAGS) -c $< -o $(BUILD)/apps/$*.o
	$(CC) $(AFLAGS) -c sdk/cwlib.c -o $(BUILD)/apps/$*-cwlib.o
	ld -m $(LDEMU) -pie --no-dynamic-linker -z norelro -z noexecstack --hash-style=sysv -e app_main \
	   $(BUILD)/apps/$*.o $(BUILD)/apps/$*-cwlib.o -o $@
	strip $@

$(BUILD)/repo.bin: tools/mkrepo.py $(APPELF) $(WINEXE) apps/*/manifest.ini winapps/*/manifest.ini build/assets.h
	python3 tools/mkrepo.py $@ build/icons $(foreach a,$(APPS),apps/$(a):$(BUILD)/apps/$(a).elf) \
	    $(foreach a,$(WINAPPS),winapps/$(a):$(BUILD)/winapps/$(a).exe)

$(BUILD)/chamows.elf: $(KOBJ) kernel/arch/$(ARCH)/linker.ld
	ld -m $(LDEMU) -T kernel/arch/$(ARCH)/linker.ld -o $@ $(KOBJ)

$(BUILD)/chamows.bin: $(BUILD)/chamows.elf
	objcopy -O binary $< $@
	@echo "Listo: $@ ($$(du -k $@ | cut -f1) KB)"

# An empty 64 MB pendrive formatted FAT32, for testing USB storage in QEMU.
$(BUILD)/pendrive.img:
	@mkdir -p $(BUILD)
	python3 tools/mkfat32.py $@ 64

run: $(BUILD)/chamows.bin $(if $(QEMU_USB),$(BUILD)/pendrive.img)
	$(QEMU) -kernel $< -vga std -m 256 -serial stdio $(QEMU_USB)

test: $(BUILD)/chamows.bin $(if $(QEMU_USB),$(BUILD)/pendrive.img)
	timeout 30 $(QEMU) -kernel $< -vga std -m 256 -display none -serial file:$(BUILD)/serial.log $(QEMU_USB) & \
	for i in $$(seq 1 25); do sleep 1; grep -q "escritorio listo" $(BUILD)/serial.log 2>/dev/null && break; done; \
	kill %1 2>/dev/null; cat $(BUILD)/serial.log; grep -q "escritorio listo" $(BUILD)/serial.log

web:
	$(MAKE) ARCH=i386
	base64 -w0 build/i386/chamows.bin > web/chamows.b64.txt
	@echo "Abre la página con: python3 -m http.server -d web 8000"

clean:
	rm -rf build

.PHONY: all both run test web clean
