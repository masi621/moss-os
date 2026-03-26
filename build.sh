#!/usr/bin/env bash
# mOSs build script fit for the current repository layout
# Current source tree assumptions:
#   sdk/shell/libmoss.c
#   sdk/shell/moss.h
#   shell/moss_shell.c shell/moss_renderer.c shell/moss_wm.c
#   services/moss_bridge.c
#   rootfs/usr/local/bin/moss_fetch.c
#   boot/grub/grub.cfg and boot/grub/moss_theme.txt
#   init (repo root)

set -Eeuo pipefail
IFS=$'\n\t'

KERNEL_VERSION="${KERNEL_VERSION:-6.6.103}"
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}"
SPLEEN_VERSION="${SPLEEN_VERSION:-2.2.0}"
KERNEL_URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KERNEL_VERSION}.tar.xz"
SPLEEN_URL="https://github.com/fcambus/spleen/releases/download/${SPLEEN_VERSION}/spleen-${SPLEEN_VERSION}.tar.gz"
BUSYBOX_URLS=(
  "https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2"
  "https://repository.timesys.com/buildsources/b/busybox/busybox-${BUSYBOX_VERSION}/busybox-${BUSYBOX_VERSION}.tar.bz2"
  "https://download.iopsys.eu/iopsys/mirror/busybox-${BUSYBOX_VERSION}.tar.bz2"
)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${WORK_DIR:-$HOME/moss-build}"
ROOTFS_DIR="${WORK_DIR}/rootfs"
ISO_STAGE="${WORK_DIR}/iso"
LOG_DIR="${WORK_DIR}/logs"
KERNEL_SRC="${WORK_DIR}/linux-${KERNEL_VERSION}"
BUSYBOX_SRC="${WORK_DIR}/busybox-${BUSYBOX_VERSION}"
OUTPUT="${OUTPUT:-${SCRIPT_DIR}/moss-native.iso}"
JOBS="${JOBS:-2}"

ENABLE_LOG=0
LOG_FILE=""
SKIP_KERNEL=0
SKIP_BUSYBOX=0
DO_CLEAN=0
DO_REBUILD=0
DEBUG=0

R='\033[0;31m'; G='\033[0;32m'; Y='\033[1;33m'; C='\033[0;36m'; B='\033[1m'; NC='\033[0m'
log()  { echo -e "${G}[mOSs]${NC} $*"; }
warn() { echo -e "${Y}[WARN]${NC} $*"; }
err()  { echo -e "${R}[ERR ]${NC} $*" >&2; exit 1; }
step() { echo -e "\n${B}${C}══ $* ══${NC}\n"; }

timestamp() { date '+%m-%d_%H-%M'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clean)        DO_CLEAN=1; shift ;;
    --rebuild)      DO_CLEAN=1; DO_REBUILD=1; shift ;;
    --skip-kernel)  SKIP_KERNEL=1; shift ;;
    --skip-busybox) SKIP_BUSYBOX=1; shift ;;
    --jobs)         JOBS="${2:?missing job count}"; shift 2 ;;
    --output)       OUTPUT="${2:?missing output path}"; shift 2 ;;
    --log)          ENABLE_LOG=1; shift ;;
    --log-file)     ENABLE_LOG=1; LOG_FILE="${2:?missing log path}"; shift 2 ;;
    --debug)        DEBUG=1; export PS4='+ [${BASH_SOURCE##*/}:${LINENO}] '; set -x; shift ;;
    *)
      err "Unknown option: $1
Valid: --clean --rebuild --skip-kernel --skip-busybox --jobs N --output FILE --log --log-file FILE --debug"
      ;;
  esac
done

# Keep host flag pollution out of builds
unset CFLAGS CPPFLAGS CXXFLAGS LDFLAGS OBJCFLAGS GCC_SPECS COMPILER_PATH LIBRARY_PATH CPATH
unset C_INCLUDE_PATH CPLUS_INCLUDE_PATH KCFLAGS HOSTCFLAGS KBUILD_CFLAGS KBUILD_CPPFLAGS MAKEFLAGS
export LC_ALL=C
export LANG=C

[[ $EUID -eq 0 ]] && err "Do NOT run this script as root."

if [[ $DO_CLEAN -eq 1 ]]; then
  rm -rf "${WORK_DIR}"
  log "Cleaned ${WORK_DIR}"
  [[ $DO_REBUILD -eq 0 ]] && exit 0
fi

mkdir -p "${WORK_DIR}" "${ROOTFS_DIR}" "${ISO_STAGE}/boot/grub" "${LOG_DIR}"

if [[ $ENABLE_LOG -eq 1 ]]; then
  [[ -n "${LOG_FILE}" ]] || LOG_FILE="${LOG_DIR}/build_$(timestamp).log"
  mkdir -p "$(dirname "$LOG_FILE")"
  exec > >(awk '{ print strftime("[%H:%M:%S]"), $0; fflush(); }' | tee -a "$LOG_FILE") 2>&1
  log "Logging to ${LOG_FILE}"
fi

HOST_GCC="$(command -v gcc)"
HOST_LDD="$(command -v ldd)"
HOST_READELF="$(command -v readelf)"

run_gcc() {
  env -u CFLAGS -u CPPFLAGS -u CXXFLAGS -u LDFLAGS -u OBJCFLAGS \
      -u GCC_SPECS -u COMPILER_PATH -u LIBRARY_PATH -u CPATH \
      -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH \
      "${HOST_GCC}" "$@"
}

copy_runtime_deps() {
  local bin="$1"
  [[ -e "$bin" ]] || return 0

  local interp=""
  interp="$("${HOST_READELF}" -l "$bin" 2>/dev/null | awk '/Requesting program interpreter/ {gsub(/[\[\]]/,"",$NF); print $NF; exit}' || true)"
  if [[ -n "${interp}" && -e "${interp}" ]]; then
    mkdir -p "${ROOTFS_DIR}$(dirname "${interp}")"
    cp -L "${interp}" "${ROOTFS_DIR}${interp}"
  fi

  if "${HOST_LDD}" "$bin" >/dev/null 2>&1; then
    "${HOST_LDD}" "$bin" | awk '/=> \// {print $(NF-1)} /^\// {print $1}' | while read -r lib; do
      [[ -n "${lib}" && -e "${lib}" ]] || continue
      mkdir -p "${ROOTFS_DIR}$(dirname "${lib}")"
      cp -L "${lib}" "${ROOTFS_DIR}${lib}"
    done
  fi
}

copy_terminfo_entry() {
  local term="$1"
  local src=""
  for base in /usr/share/terminfo /lib/terminfo /etc/terminfo; do
    if [[ -f "${base}/${term:0:1}/${term}" ]]; then
      src="${base}/${term:0:1}/${term}"
      break
    fi
  done
  if [[ -n "${src}" ]]; then
    mkdir -p "${ROOTFS_DIR}/usr/share/terminfo/${term:0:1}"
    cp -L "${src}" "${ROOTFS_DIR}/usr/share/terminfo/${term:0:1}/${term}"
  fi
}

require_file() {
  [[ -f "$1" ]] || err "Required file missing: $1"
}

# Resolve your current repo layout cleanly.
if [[ -f "${SCRIPT_DIR}/sdk/shell/libmoss.c" && -f "${SCRIPT_DIR}/sdk/shell/moss.h" ]]; then
  SDK_SRC_DIR="${SCRIPT_DIR}/sdk/shell"
elif [[ -f "${SCRIPT_DIR}/sdk/libmoss.c" && -f "${SCRIPT_DIR}/sdk/moss.h" ]]; then
  SDK_SRC_DIR="${SCRIPT_DIR}/sdk"
else
  err "Could not find libmoss.c + moss.h under sdk/ or sdk/shell/"
fi

LIBMOSS_C="${SDK_SRC_DIR}/libmoss.c"
MOSS_H="${SDK_SRC_DIR}/moss.h"
MOSS_SHELL_C="${SCRIPT_DIR}/shell/moss_shell.c"
MOSS_RENDERER_C="${SCRIPT_DIR}/shell/moss_renderer.c"
MOSS_WM_C="${SCRIPT_DIR}/shell/moss_wm.c"
MOSS_BRIDGE_C="${SCRIPT_DIR}/services/moss_bridge.c"
MOSS_FETCH_C="${SCRIPT_DIR}/rootfs/usr/local/bin/moss_fetch.c"
INIT_FILE="${SCRIPT_DIR}/init"
ART_JSON="${SCRIPT_DIR}/ascii_art.json"
[[ -f "${ART_JSON}" ]] || ART_JSON="${SCRIPT_DIR}/assets/ascii_art.json"
ART_EMBED_H="${SCRIPT_DIR}/shell/moss_ascii_art_embed.h"
SPLEEN_SRC="${WORK_DIR}/spleen-${SPLEEN_VERSION}"
GRUB_CFG="${SCRIPT_DIR}/boot/grub/grub.cfg"
GRUB_THEME="${SCRIPT_DIR}/boot/grub/moss_theme.txt"

require_file "${LIBMOSS_C}"
require_file "${MOSS_H}"
require_file "${MOSS_SHELL_C}"
require_file "${MOSS_RENDERER_C}"
require_file "${MOSS_WM_C}"
require_file "${MOSS_BRIDGE_C}"
require_file "${MOSS_FETCH_C}"
require_file "${INIT_FILE}"

step "Checking host dependencies"
MISSING=()
for cmd in gcc make xorriso cpio gzip grub-mkrescue wget tar bzip2 xz readelf ldd python3; do
  if command -v "$cmd" >/dev/null 2>&1; then
    log "  ✓ $cmd"
  else
    warn "  ✗ $cmd"
    MISSING+=("$cmd")
  fi
done
[[ ${#MISSING[@]} -eq 0 ]] || err "Missing required tools: ${MISSING[*]}"

step "Downloading sources"
dl() {
  local url="$1" out="$2"
  if [[ -f "$out" ]]; then
    log "  Cached: $(basename "$out")"
  else
    log "  Fetching: $(basename "$out")"
    wget --tries=3 --timeout=20 --waitretry=3 -O "$out" "$url"
  fi
}
dl "${KERNEL_URL}"  "${WORK_DIR}/linux-${KERNEL_VERSION}.tar.xz"

if [[ ! -f "${WORK_DIR}/busybox-${BUSYBOX_VERSION}.tar.bz2" ]]; then
  got_busybox=0
  for url in "${BUSYBOX_URLS[@]}"; do
    log "  Trying BusyBox source: $url"
    if wget --tries=2 --timeout=20 --waitretry=3 -O "${WORK_DIR}/busybox-${BUSYBOX_VERSION}.tar.bz2" "$url"; then
      got_busybox=1
      break
    fi
    rm -f "${WORK_DIR}/busybox-${BUSYBOX_VERSION}.tar.bz2"
    warn "  BusyBox download failed from: $url"
  done
  [[ $got_busybox -eq 1 ]] || err "Could not download BusyBox ${BUSYBOX_VERSION} from any configured source"
else
  log "  Cached: busybox-${BUSYBOX_VERSION}.tar.bz2"
fi
[[ -d "${KERNEL_SRC}"  ]] || tar -xJf "${WORK_DIR}/linux-${KERNEL_VERSION}.tar.xz"  -C "${WORK_DIR}"
[[ -d "${BUSYBOX_SRC}" ]] || tar -xjf "${WORK_DIR}/busybox-${BUSYBOX_VERSION}.tar.bz2" -C "${WORK_DIR}"
dl "${SPLEEN_URL}" "${WORK_DIR}/spleen-${SPLEEN_VERSION}.tar.gz"
if [[ ! -d "${SPLEEN_SRC}" ]]; then
  mkdir -p "${SPLEEN_SRC}"
  tar -xzf "${WORK_DIR}/spleen-${SPLEEN_VERSION}.tar.gz" -C "${SPLEEN_SRC}" --strip-components=1 || true
fi

if [[ -f "${ART_JSON}" ]]; then
  log "  Using checked-in art from ${ART_JSON}"
else
  warn "ascii_art.json not found; ferris will use the checked-in embedded header"
fi

# Fresh staging each build, so you never pack stale junk.
rm -rf "${ROOTFS_DIR}" "${ISO_STAGE}"
mkdir -p "${ROOTFS_DIR}" "${ISO_STAGE}/boot/grub" "${LOG_DIR}"

step "Building Linux kernel ${KERNEL_VERSION}"
KERNEL_CC='gcc -std=gnu11'
KERNEL_HOSTCC='gcc -std=gnu11'
KERNEL_HOSTCFLAGS='-Wno-error=implicit-function-declaration -Wno-error=implicit-int'
KERNEL_KCFLAGS='-Wno-error'

if [[ $SKIP_KERNEL -eq 1 && -f "${WORK_DIR}/linux-${KERNEL_VERSION}/arch/x86/boot/bzImage" ]]; then
  log "Skipping kernel (--skip-kernel)"
else
  cd "${KERNEL_SRC}"
  make CC="${KERNEL_CC}" HOSTCC="${KERNEL_HOSTCC}" HOSTCFLAGS="${KERNEL_HOSTCFLAGS}" KCFLAGS="${KERNEL_KCFLAGS}" mrproper
  make CC="${KERNEL_CC}" HOSTCC="${KERNEL_HOSTCC}" HOSTCFLAGS="${KERNEL_HOSTCFLAGS}" KCFLAGS="${KERNEL_KCFLAGS}" x86_64_defconfig
  scripts/config --disable CONFIG_X86_KERNEL_IBT 2>/dev/null || true
  scripts/config --disable CONFIG_EFI_STUB 2>/dev/null || true
  for f in \
    CONFIG_DRM CONFIG_DRM_KMS_HELPER CONFIG_DRM_SIMPLEDRM CONFIG_FB CONFIG_FB_EFI CONFIG_FB_SIMPLE CONFIG_FB_VESA \
    CONFIG_FRAMEBUFFER_CONSOLE CONFIG_VGA_CONSOLE CONFIG_INPUT CONFIG_INPUT_EVDEV CONFIG_INPUT_MOUSEDEV \
    CONFIG_INPUT_KEYBOARD CONFIG_INPUT_MOUSE CONFIG_MOUSE_PS2 CONFIG_SERIO CONFIG_SERIO_I8042 CONFIG_SERIO_SERPORT \
    CONFIG_HID CONFIG_HID_GENERIC CONFIG_USB_HID CONFIG_USB_TABLET CONFIG_DRM_BOCHS CONFIG_DRM_QXL CONFIG_DRM_VMWGFX \
    CONFIG_SOUND CONFIG_SND CONFIG_SND_HDA_INTEL CONFIG_SND_USB_AUDIO CONFIG_TMPFS \
    CONFIG_DEVTMPFS CONFIG_DEVTMPFS_MOUNT CONFIG_PROC_FS CONFIG_SYSFS CONFIG_OVERLAY_FS \
    CONFIG_SQUASHFS CONFIG_SQUASHFS_ZSTD CONFIG_EXT4_FS CONFIG_BLK_DEV_LOOP \
    CONFIG_SERIAL_8250 CONFIG_SERIAL_8250_CONSOLE CONFIG_VIRTIO_PCI CONFIG_VIRTIO_BLK \
    CONFIG_VIRTIO_NET CONFIG_VIRTIO_INPUT
  do
    scripts/config --enable "$f" 2>/dev/null || true
  done
  # Avoid giant GPU trees you do not need for QEMU bring-up.
  for f in CONFIG_DRM_AMDGPU CONFIG_DRM_I915 CONFIG_DRM_VIRTIO_GPU; do
    scripts/config --disable "$f" 2>/dev/null || true
  done
  if ! make CC="${KERNEL_CC}" HOSTCC="${KERNEL_HOSTCC}" HOSTCFLAGS="${KERNEL_HOSTCFLAGS}" KCFLAGS="${KERNEL_KCFLAGS}" olddefconfig; then
    err "Kernel olddefconfig failed"
  fi
  if ! make CC="${KERNEL_CC}" HOSTCC="${KERNEL_HOSTCC}" HOSTCFLAGS="${KERNEL_HOSTCFLAGS}" KCFLAGS="${KERNEL_KCFLAGS}" -j"${JOBS}" bzImage 2>&1 | tee "${LOG_DIR}/kernel-build.log"; then
    err "Kernel build failed. See ${LOG_DIR}/kernel-build.log"
  fi
  [[ -f arch/x86/boot/bzImage ]] || err "Kernel bzImage not produced"
  cd "${SCRIPT_DIR}"
fi
cp "${KERNEL_SRC}/arch/x86/boot/bzImage" "${ISO_STAGE}/boot/vmlinuz"

step "Building BusyBox ${BUSYBOX_VERSION}"
BB_CC='gcc -std=gnu11'
BB_CFLAGS='-Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion'

if [[ $SKIP_BUSYBOX -eq 1 && -f "${BUSYBOX_SRC}/busybox" ]]; then
  log "Skipping BusyBox (--skip-busybox)"
else
  cd "${BUSYBOX_SRC}"
  make mrproper
  make CC="${BB_CC}" defconfig
  scripts/config --enable CONFIG_STATIC 2>/dev/null || sed -i 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' .config
  scripts/config --disable CONFIG_FEATURE_HAVE_RPC 2>/dev/null || true
  scripts/config --disable CONFIG_IPCALC 2>/dev/null || true
  scripts/config --disable CONFIG_TC 2>/dev/null || sed -i 's/CONFIG_TC=y/# CONFIG_TC is not set/' .config
  # Important: do NOT run olddefconfig here. It can re-enter kconfig prompting on this BusyBox tree.
  if ! make -j"${JOBS}" CC="${BB_CC}" CFLAGS="${BB_CFLAGS}" CFLAGS_busybox="${BB_CFLAGS}" 2>&1 | tee "${LOG_DIR}/busybox-build.log"; then
    err "BusyBox build failed. See ${LOG_DIR}/busybox-build.log"
  fi
  [[ -f busybox ]] || err "BusyBox binary not produced"
  cd "${SCRIPT_DIR}"
fi

step "Compiling mOSs native components"
mkdir -p "${ROOTFS_DIR}/bin" "${ROOTFS_DIR}/lib" "${ROOTFS_DIR}/usr/bin"

INCLUDES=(-I"${SDK_SRC_DIR}" -I"${SCRIPT_DIR}/shell")

run_gcc -O2 -march=x86-64 -pipe -fstack-protector-strong -D_GNU_SOURCE \
  -fPIC -shared "${LIBMOSS_C}" "${INCLUDES[@]}" \
  -lncurses -lpanel -lsqlite3 -lpthread -lm \
  -o "${ROOTFS_DIR}/lib/libmoss.so.1" 2>"${LOG_DIR}/libmoss.so.err" \
  || err "libmoss.so build failed. See ${LOG_DIR}/libmoss.so.err"
ln -sf libmoss.so.1 "${ROOTFS_DIR}/lib/libmoss.so"

run_gcc -O2 -march=x86-64 -pipe -fstack-protector-strong -D_GNU_SOURCE \
  -Wl,-z,now -Wl,-z,relro \
  "${MOSS_SHELL_C}" "${MOSS_RENDERER_C}" "${MOSS_WM_C}" "${INCLUDES[@]}" \
  -lncurses -lpanel -lm -lpthread \
  -L"${ROOTFS_DIR}/lib" -lmoss -Wl,-rpath,/lib \
  -o "${ROOTFS_DIR}/bin/moss-shell" 2>"${LOG_DIR}/moss-shell.err" \
  || err "moss-shell build failed. See ${LOG_DIR}/moss-shell.err"

run_gcc -O2 -march=x86-64 -pipe -fstack-protector-strong -D_GNU_SOURCE \
  -Wl,-z,now -Wl,-z,relro \
  "${MOSS_BRIDGE_C}" -I"${SDK_SRC_DIR}" \
  -lsqlite3 -lm -lpthread \
  -o "${ROOTFS_DIR}/bin/moss-bridge" 2>"${LOG_DIR}/moss-bridge.err" \
  || warn "moss-bridge build failed (non-fatal) — see ${LOG_DIR}/moss-bridge.err"

run_gcc -O2 -march=x86-64 -pipe -fstack-protector-strong -D_GNU_SOURCE \
  -Wl,-z,now -Wl,-z,relro \
  "${MOSS_FETCH_C}" -I"${SDK_SRC_DIR}" \
  -lncurses -lpanel -lm -L"${ROOTFS_DIR}/lib" -lmoss -Wl,-rpath,/lib \
  -o "${ROOTFS_DIR}/bin/moss-fetch" 2>"${LOG_DIR}/moss-fetch.err" \
  || warn "moss-fetch build failed (non-fatal) — see ${LOG_DIR}/moss-fetch.err"

step "Assembling rootfs"
for d in \
  proc sys dev run tmp var/log var/lib/moss home/moss root \
  etc/moss etc/init.d usr/share/moss usr/share/moss/ascii usr/share/moss/sounds \
  usr/share/moss/fonts usr/share/consolefonts usr/share/man/man1 usr/share/terminfo mnt/data
do
  mkdir -p "${ROOTFS_DIR}/${d}"
done
chmod 1777 "${ROOTFS_DIR}/tmp"

install -m 755 "${BUSYBOX_SRC}/busybox" "${ROOTFS_DIR}/bin/busybox"
"${BUSYBOX_SRC}/busybox" --list | while read -r applet; do
  ln -sf busybox "${ROOTFS_DIR}/bin/${applet}" 2>/dev/null || true
done
ln -sf busybox "${ROOTFS_DIR}/bin/sh"
ln -sf busybox "${ROOTFS_DIR}/bin/ash"

for tool in setfont; do
  if command -v "$tool" >/dev/null 2>&1; then
    install -m 755 "$(command -v "$tool")" "${ROOTFS_DIR}/bin/${tool}"
    copy_runtime_deps "${ROOTFS_DIR}/bin/${tool}"
  fi
done

copy_runtime_deps "${ROOTFS_DIR}/bin/busybox"
copy_runtime_deps "${ROOTFS_DIR}/lib/libmoss.so.1"
copy_runtime_deps "${ROOTFS_DIR}/bin/moss-shell"
copy_runtime_deps "${ROOTFS_DIR}/bin/moss-bridge"
copy_runtime_deps "${ROOTFS_DIR}/bin/moss-fetch"

copy_terminfo_entry linux
copy_terminfo_entry vt100
copy_terminfo_entry xterm
copy_terminfo_entry xterm-256color

# Copy your project skeleton files from repo rootfs/ into staged rootfs.
cp -a "${SCRIPT_DIR}/rootfs/." "${ROOTFS_DIR}/" 2>/dev/null || true

# Copy SDK headers into image for your own tooling/debug.
cp -f "${MOSS_H}" "${ROOTFS_DIR}/usr/share/moss/" 2>/dev/null || true

[[ -f "${SCRIPT_DIR}/assets/chime.mp3"        ]] && cp "${SCRIPT_DIR}/assets/chime.mp3"        "${ROOTFS_DIR}/usr/share/moss/sounds/"
[[ -f "${ART_JSON}"                         ]] && cp "${ART_JSON}"                         "${ROOTFS_DIR}/usr/share/moss/ascii/ascii_art.json"
[[ -f "${SCRIPT_DIR}/assets/bg.png"         ]] && cp "${SCRIPT_DIR}/assets/bg.png"         "${ROOTFS_DIR}/usr/share/moss/"
[[ -f "${SCRIPT_DIR}/assets/eye.png"        ]] && cp "${SCRIPT_DIR}/assets/eye.png"        "${ROOTFS_DIR}/usr/share/moss/"
[[ -f "${SCRIPT_DIR}/assets/wall-green.png" ]] && cp "${SCRIPT_DIR}/assets/wall-green.png" "${ROOTFS_DIR}/usr/share/moss/wallpaper.png"
[[ -f "${SCRIPT_DIR}/assets/newLOGO.png"    ]] && cp "${SCRIPT_DIR}/assets/newLOGO.png"    "${ROOTFS_DIR}/usr/share/moss/bootlogo.png"

cat > "${ROOTFS_DIR}/etc/moss/config.json" <<'JSON'
{
  "version": "1.0.0",
  "hostname": "mOSs",
  "terminal_color": "#00ff00",
  "crt_effect": "light",
  "background_enabled": true,
  "startup_mode": "shell",
  "audio_enabled": true,
  "bridge_socket": "/run/moss-bridge.sock",
  "db_path": "/var/lib/moss/moss.db",
  "ascii_path": "/usr/share/moss/ascii/ascii_art.json",
  "chime_path": "/usr/share/moss/sounds/chime.mp3",
  "persist_partition": "/dev/sda2",
  "log_level": "warn"
}
JSON

cat > "${ROOTFS_DIR}/etc/passwd" <<'PASSWD'
root:x:0:0:root:/root:/bin/sh
moss:x:1000:1000:mOSs:/home/moss:/bin/moss-shell
guest:x:1001:1001:Guest:/tmp:/bin/moss-shell
PASSWD

cat > "${ROOTFS_DIR}/etc/group" <<'GROUP'
root:x:0:root
moss:x:1000:moss
guest:x:1001:guest
audio:x:29:moss
video:x:44:moss
input:x:24:moss
GROUP

echo "mOSs" > "${ROOTFS_DIR}/etc/hostname"
printf "127.0.0.1\tlocalhost\n127.0.0.1\tmOSs\n" > "${ROOTFS_DIR}/etc/hosts"

[[ -f "${SCRIPT_DIR}/docs/man/moss.1" ]] && gzip -c "${SCRIPT_DIR}/docs/man/moss.1" > "${ROOTFS_DIR}/usr/share/man/man1/moss.1.gz"

install -m 755 "${INIT_FILE}" "${ROOTFS_DIR}/init"
[[ -f "${ART_JSON}" ]] && install -m 644 "${ART_JSON}" "${ROOTFS_DIR}/usr/share/moss/ascii_art.json"
[[ -f "${ART_JSON}" ]] && install -m 644 "${ART_JSON}" "${ROOTFS_DIR}/usr/share/moss/ascii/ascii_art.json"
if [[ -d "${SPLEEN_SRC}" ]]; then
  while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    install -m 644 "$f" "${ROOTFS_DIR}/usr/share/consolefonts/$(basename "$f")"
  done < <(find "${SPLEEN_SRC}" -maxdepth 1 -type f \( -name 'spleen-8x16.psfu.gz' -o -name 'spleen-8x16.psfu' -o -name 'spleen-16x32.psfu.gz' -o -name 'spleen-16x32.psfu' \))

  while IFS= read -r f; do
    [[ -n "$f" ]] || continue
    install -m 644 "$f" "${ROOTFS_DIR}/usr/share/moss/fonts/$(basename "$f")"
  done < <(find "${SPLEEN_SRC}" -maxdepth 1 -type f \( -name 'spleen-8x16.otf' -o -name 'spleen-16x32.otf' -o -name 'spleen-8x16.otb' -o -name 'spleen-16x32.otb' \))
fi

[ -x "${ROOTFS_DIR}/init" ] || err "/init missing or not executable"
[ -f "${ROOTFS_DIR}/bin/busybox" ] || err "/bin/busybox missing"
([ -L "${ROOTFS_DIR}/bin/sh" ] || [ -f "${ROOTFS_DIR}/bin/sh" ]) || err "/bin/sh missing"
[ -x "${ROOTFS_DIR}/bin/moss-shell" ] || err "/bin/moss-shell missing"

step "Packing initramfs"
cd "${ROOTFS_DIR}"
find . -print0 | cpio --null -ov --format=newc 2>/dev/null | gzip -9 > "${ISO_STAGE}/boot/initrd.img"
cd "${SCRIPT_DIR}"

step "Installing bootloader configuration"
mkdir -p "${ISO_STAGE}/boot/grub/themes/moss"
if [[ -f "${GRUB_CFG}" ]]; then
  cp "${GRUB_CFG}" "${ISO_STAGE}/boot/grub/grub.cfg"
else
  cat > "${ISO_STAGE}/boot/grub/grub.cfg" <<'GRUBCFG'
set timeout=3
set default=0
menuentry "mOSs-Native" {
    linux /boot/vmlinuz console=tty0 console=ttyS0,115200 init=/init loglevel=7
    initrd /boot/initrd.img
}
GRUBCFG
fi
[[ -f "${GRUB_THEME}"                        ]] && cp "${GRUB_THEME}"                        "${ISO_STAGE}/boot/grub/themes/moss/theme.txt"
[[ -f "${SCRIPT_DIR}/assets/wall-green.png" ]] && cp "${SCRIPT_DIR}/assets/wall-green.png" "${ISO_STAGE}/boot/grub/themes/moss/"
[[ -f "${SCRIPT_DIR}/assets/newLOGO.png"    ]] && cp "${SCRIPT_DIR}/assets/newLOGO.png"    "${ISO_STAGE}/boot/grub/themes/moss/"
[[ -f "${SCRIPT_DIR}/assets/bg.png"         ]] && cp "${SCRIPT_DIR}/assets/bg.png"         "${ISO_STAGE}/boot/grub/themes/moss/"
[[ -f "${SCRIPT_DIR}/assets/eye.png"        ]] && cp "${SCRIPT_DIR}/assets/eye.png"        "${ISO_STAGE}/boot/grub/themes/moss/"

step "Building ISO"
if ! grub-mkrescue \
      --output="${OUTPUT}" \
      "${ISO_STAGE}" \
      --modules="linux normal iso9660 search gfxterm gfxterm_background png" \
      2>&1 | tee "${LOG_DIR}/grub-mkrescue.log"; then
  err "grub-mkrescue failed. See ${LOG_DIR}/grub-mkrescue.log"
fi

log "ISO ready: ${OUTPUT}"
log "To test in QEMU, run: qemu-system-x86_64 -m 1024M -cdrom ${OUTPUT} -vga virtio"
