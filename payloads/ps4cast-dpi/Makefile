# PS4 Cast resident deploy agent (GoldHEN payload listening on TCP 9192).
#
# Built from THIS directory's agent.c (authenticated protocol, see
# agent_proto.h) plus the runtime of the pinned DirectPackageInstaller
# checkout (scripts/setup-payload-deps.sh): crt/syscalls/dl and ps4-libjbc.
# The previous agent came from local, uncommitted DPI changes and could not
# be rebuilt from the repository.
#
#   make                       # -> build/ps4cast-dpi.bin
#   make install               # also refresh the committed scripts/dpi-payload.bin
#   make LLVM=/usr/bin         # Linux: clang/ld.lld/llvm-objcopy from PATH dirs
#
# The binary carries AGENT_SECRET_PLACEHOLDER; scripts/push-goldhen-dpi.py
# patches a fresh secret into it before sending, and the agent refuses to run
# unpatched.
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
DPI := $(ROOT)/third_party/DirectPackageInstaller/Payload
LLVM ?= /opt/homebrew/opt/llvm/bin
LLD ?= $(if $(filter /opt/homebrew/opt/llvm/bin,$(LLVM)),/opt/homebrew/opt/lld/bin,$(LLVM))
CC := $(LLVM)/clang
LD := $(LLD)/ld.lld
OBJCOPY := $(LLVM)/llvm-objcopy
YASM := yasm
CFLAGS := --target=x86_64-pc-freebsd12-elf -fuse-ld=$(LD) -g -O1 -mcmodel=small -isystem $(DPI)/freebsd-headers -I$(DPI) -I$(DPI)/ps4-libjbc -I. -nostdinc -nostdlib -fno-stack-protector -static -fPIE -ffreestanding -fno-builtin -Wall
CFLAGS += -DAGENT_PORT=9192
LDFLAGS := -Wl,-T,$(ROOT)/payloads/ps4cast-control/payload.ld -Wl,-gc-sections
LIB := build/lib.a
JBC := $(wildcard $(DPI)/ps4-libjbc/*.c)
SRCS := agent.c agent_proto.c sha256.c jb.c

.PHONY: all install clean

all: build/ps4cast-dpi.bin

install: build/ps4cast-dpi.bin
	cp build/ps4cast-dpi.bin $(ROOT)/scripts/dpi-payload.bin

build:
	mkdir -p build

build/crt.o: $(DPI)/lib/crt.asm | build
	$(YASM) -f elf64 $< -o $@

build/syscalls.o: $(DPI)/lib/syscalls.asm | build
	$(YASM) -f elf64 $< -o $@

build/dl.o: $(DPI)/lib/dl.c | build
	$(CC) --target=x86_64-pc-freebsd12-elf -c -isystem $(DPI)/freebsd-headers -nostdinc -fno-stack-protector $< -o $@ -fPIE -ffreestanding

$(LIB): build/crt.o build/dl.o build/syscalls.o | build
	$(LD) -r $^ -o $@

build/ps4cast-dpi.elf: $(SRCS) agent_proto.h sha256.h $(LIB) | build
	$(CC) $(CFLAGS) $(LIB) $(SRCS) $(JBC) $(LDFLAGS) -o $@

build/ps4cast-dpi.bin: build/ps4cast-dpi.elf
	$(OBJCOPY) $< --only-section .text --only-section .data --only-section .bss --only-section .rodata -O binary $@

clean:
	rm -rf build
