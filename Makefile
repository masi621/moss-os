# =============================================================================
#  mOSs-Native :: Makefile
#  Orchestrates: kernel → BusyBox → services → rootfs → ISO
# =============================================================================

.PHONY: all iso services clean clean-all check-deps dev qemu flash help

SHELL        := /usr/bin/env bash
OUTPUT       ?= moss-native.iso
ARCH         ?= x86_64
JOBS         ?= $(shell nproc)
KERNEL_VER   ?= 6.6.103
BUSYBOX_VER  ?= 1.36.1
WORK_DIR     ?= /tmp/moss-build

# ── Colors ────────────────────────────────────────────────────────────────────
G  := \033[0;32m
Y  := \033[1;33m
C  := \033[0;36m
B  := \033[1m
NC := \033[0m

all: check-deps services iso
	@printf "$(B)$(G)✓ mOSs-Native build complete: $(OUTPUT)$(NC)\n"

# ── Dependency check ──────────────────────────────────────────────────────────
check-deps:
	@printf "$(C)Checking build dependencies...$(NC)\n"
	@for dep in gcc make xorriso cpio gzip grub-mkrescue wget; do \
	  if command -v $$dep &>/dev/null; then \
	    printf "  $(G)✓$(NC) $$dep\n"; \
	  else \
	    printf "  $(Y)✗ $$dep (missing)$(NC)\n"; \
	  fi; \
	done
	@for lib in libncurses-dev libsqlite3-dev; do \
	  if dpkg -l $$lib 2>/dev/null | grep -q '^ii'; then \
	    printf "  $(G)✓$(NC) $$lib\n"; \
	  else \
	    printf "  $(Y)? $$lib (check manually)$(NC)\n"; \
	  fi; \
	done 2>/dev/null || true

# ── Compile services locally for dev/test ─────────────────────────────────────
services: services/moss_bridge services/moss_fetch

services/moss_bridge: services/moss_bridge.c sdk/moss.h
	@printf "$(C)Compiling moss-bridge...$(NC)\n"
	gcc -O2 -D_GNU_SOURCE services/moss_bridge.c \
	  -Isdk -lsqlite3 -lpthread -lm \
	  -o services/moss_bridge \
	  && printf "  $(G)✓$(NC) moss-bridge\n" \
	  || printf "  $(Y)✗ moss-bridge (check libsqlite3-dev)$(NC)\n"

services/moss_fetch: rootfs/usr/local/bin/moss_fetch.c sdk/moss.h
	@printf "$(C)Compiling moss-fetch...$(NC)\n"
	gcc -O2 -D_GNU_SOURCE rootfs/usr/local/bin/moss_fetch.c \
	  -Isdk -lm \
	  -o services/moss_fetch \
	  && printf "  $(G)✓$(NC) moss-fetch\n" \
	  || printf "  $(Y)✗ moss-fetch$(NC)\n"

# ── ISO ───────────────────────────────────────────────────────────────────────
iso:
	@printf "$(C)Building ISO...$(NC)\n"
	bash build.sh \
	  --output "$(OUTPUT)" \
	  --jobs "$(JOBS)"

# ── ISO: resume after partial build (kernel already compiled) ─────────────────
iso-resume:
	@printf "$(C)Resuming ISO build (skipping kernel)...$(NC)\n"
	bash build.sh \
	  --output "$(OUTPUT)" \
	  --jobs "$(JOBS)" \
	  --skip-kernel

# ── Dev mode ──────────────────────────────────────────────────────────────────
dev: services
	@printf "$(C)Starting mOSs dev services...$(NC)\n"
	@./services/moss_bridge --socket /tmp/moss-bridge.sock &
	@printf "$(G)moss-bridge running on /tmp/moss-bridge.sock$(NC)\n"
	@printf "Test:  echo '{\"cmd\":\"system\"}' | nc -U /tmp/moss-bridge.sock\n"
	@printf "Kill:  pkill moss-bridge\n"

# ── QEMU ──────────────────────────────────────────────────────────────────────
qemu: $(OUTPUT)
	@printf "$(C)Launching QEMU...$(NC)\n"
	qemu-system-x86_64 \
	  -m 512M \
	  -cdrom $(OUTPUT) \
	  -vga virtio \
	  -enable-kvm \
	  -serial stdio \
	  -audiodev pa,id=snd0 \
	  -machine pcspk-audiodev=snd0 \
	  2>/dev/null || \
	qemu-system-x86_64 \
	  -m 512M \
	  -cdrom $(OUTPUT) \
	  -vga virtio \
	  -serial stdio

# ── Flash ─────────────────────────────────────────────────────────────────────
flash: $(OUTPUT)
	@[ -n "$(DEVICE)" ] || { printf "$(Y)Usage: make flash DEVICE=/dev/sdX$(NC)\n"; exit 1; }
	@printf "$(Y)Flashing $(OUTPUT) → $(DEVICE)$(NC)\n"
	sudo dd if=$(OUTPUT) of=$(DEVICE) bs=4M status=progress conv=fsync
	sync
	@printf "$(G)✓ Flash complete$(NC)\n"

# ── man page ──────────────────────────────────────────────────────────────────
man:
	@man docs/man/moss.1 2>/dev/null || \
	nroff -man docs/man/moss.1 | less

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	@printf "$(Y)Cleaning build artifacts...$(NC)\n"
	rm -f $(OUTPUT) services/moss_bridge services/moss_fetch
	find . -name '*.o' -delete

clean-all: clean
	@printf "$(Y)Removing work directory $(WORK_DIR)...$(NC)\n"
	sudo rm -rf $(WORK_DIR)

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@printf "\n$(B)mOSs-Native Build System$(NC)\n\n"
	@printf "  $(G)make$(NC)              Build everything (services + ISO)\n"
	@printf "  $(G)make services$(NC)     Compile bridge + fetch locally\n"
	@printf "  $(G)make iso$(NC)          Build bootable ISO\n"
	@printf "  $(G)make dev$(NC)          Run bridge locally for testing\n"
	@printf "  $(G)make qemu$(NC)         Launch in QEMU\n"
	@printf "  $(G)make flash$(NC)        Flash to USB  (DEVICE=/dev/sdX)\n"
	@printf "  $(G)make man$(NC)          View man page\n"
	@printf "  $(G)make clean$(NC)        Remove build artifacts\n"
	@printf "  $(G)make clean-all$(NC)    Remove everything\n"
	@printf "\n  Variables:\n"
	@printf "    OUTPUT=$(OUTPUT)  ARCH=$(ARCH)  JOBS=$(JOBS)\n"
	@printf "    KERNEL_VER=$(KERNEL_VER)  BUSYBOX_VER=$(BUSYBOX_VER)\n\n"
