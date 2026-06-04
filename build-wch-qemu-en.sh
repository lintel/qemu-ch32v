#!/usr/bin/env bash
# Build QEMU 10.2.2 (riscv32-softmmu only) from the official source tarball
# for CH32V317 full-system emulation.
#
# Workflow:
#   1. Download / verify qemu-<version>.tar.xz
#   2. Extract to build/qemu-<version>/
#   3. Overlay CH32 machine model sources from qemu-overlay/<version>/
#   4. Apply patches/qemu/*.patch
#   5. configure → make → make install
#
# Default paths:
#   Download cache:  <repo>/downloads/archives/
#   Source dir:      <repo>/build/qemu-<version>/
#   Build dir:       <repo>/build/qemu-<version>-obj/
#   Install prefix:  <repo>/dist/qemu-<version>-riscv32/
#
# This script lives at the repository root; see docs/QEMU-CH32V317本地测试环境.md

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# --- Version and download URL (overridable via environment variables) ---
WCH_QEMU_VERSION=${WCH_QEMU_VERSION:-10.2.2}
WCH_QEMU_URL=${WCH_QEMU_URL:-https://download.qemu.org/qemu-${WCH_QEMU_VERSION}.tar.xz}
# Matches https://download.qemu.org/qemu-10.2.2.tar.xz (override with WCH_QEMU_SHA256 or skip with --skip-archive-sha256)
WCH_QEMU_SHA256=${WCH_QEMU_SHA256:-784b296ff29c1417aa72323abcb2d2ea9ab9771724f577dcd785c3b04f21e176}

PREFIX_DEFAULT="${REPO_ROOT}/dist/qemu-${WCH_QEMU_VERSION}-riscv32"
DOWNLOADS_DEFAULT="${REPO_ROOT}/downloads/archives"
BUILD_TOP_DEFAULT="${REPO_ROOT}/build"

INSTALL_DEB_DEPS=1
SKIP_ARCHIVE_SHA256=0
FORCE_REBUILD=0
CLEAN_BUILD=0
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

WCH_DEB_PKGS_QEMU=(
  build-essential
  git
  python3
  ninja-build
  flex
  bison
  pkg-config
  libglib2.0-dev
  libpixman-1-dev
  zlib1g-dev
  libslirp-dev
)

usage() {
  cat <<EOF
Usage: $(basename "${BASH_SOURCE[0]}") [options]

Build WCH CH32V317 QEMU (riscv32-softmmu) from the official source tarball.

Default install prefix: ${PREFIX_DEFAULT}

Options:
  --prefix PATH          Installation directory
  --downloads-dir PATH   Source tarball cache directory (default: <repo>/downloads/archives)
  --build-dir PATH       Build root directory (default: <repo>/build)
  -j N | -jN             Parallel build jobs (default: \$(nproc))
  --no-install-deps      Skip apt dependency installation on Debian/Ubuntu
  --skip-archive-sha256  Skip source tarball SHA256 verification
  --force-rebuild        Force re-extraction of source (removes old build dir; implies --clean)
  --clean                Remove build directory (obj_dir) and re-run configure + full build
  -h, --help

Environment variables:
  WCH_QEMU_VERSION       QEMU version (default: 10.2.2)
  WCH_QEMU_URL           Download URL
  WCH_QEMU_SHA256        Expected SHA256 (lowercase hex)
  WCH_SKIP_ARCHIVE_SHA256=1  Same as --skip-archive-sha256
  JOBS                   Parallel build jobs
EOF
}

# --- Utility functions ---

_wch_dpkg_pkg_ok() {
  dpkg-query -W -f='${Status}' "$1" 2>/dev/null | grep -q 'ok installed'
}

_wch_is_debian_like() {
  command -v apt-get >/dev/null 2>&1 || return 1
  command -v dpkg-query >/dev/null 2>&1 || return 1
  if [[ -f /etc/debian_version ]]; then
    return 0
  fi
  [[ -f /etc/os-release ]] || return 1
  # shellcheck source=/dev/null
  source /etc/os-release
  case "${ID:-}" in
    debian|ubuntu|raspbian|linuxmint|pop) return 0 ;;
  esac
  case ",${ID_LIKE:-}," in
    *,debian,*|*,ubuntu,*) return 0 ;;
  esac
  return 1
}

_wch_apt_get() {
  if [[ "$(id -u)" -eq 0 ]]; then
    env DEBIAN_FRONTEND=noninteractive apt-get "$@"
  else
    sudo env DEBIAN_FRONTEND=noninteractive apt-get "$@"
  fi
}

_wch_ensure_debian_build_deps() {
  if [[ "$INSTALL_DEB_DEPS" -eq 0 ]] || ! _wch_is_debian_like; then
    return 0
  fi
  local missing=()
  local pkg
  for pkg in "${WCH_DEB_PKGS_QEMU[@]}"; do
    if ! _wch_dpkg_pkg_ok "$pkg"; then
      missing+=("$pkg")
    fi
  done
  if [[ ${#missing[@]} -eq 0 ]]; then
    echo "==> Debian/Ubuntu: QEMU build dependencies already satisfied"
    return 0
  fi
  echo "==> Debian/Ubuntu: Installing missing dependencies: ${missing[*]}" >&2
  if [[ "$(id -u)" -ne 0 ]] && ! command -v sudo >/dev/null 2>&1; then
    echo "Error: Not root and sudo is not available." >&2
    exit 1
  fi
  _wch_apt_get update -qq
  _wch_apt_get install -y --no-install-recommends "${missing[@]}"
  echo "==> Dependency installation complete"
}

_wch_fetch_url() {
  local url="$1" out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --connect-timeout 60 --retry 3 --retry-delay 2 -o "$out" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$out" "$url"
  else
    echo "Error: curl or wget is required" >&2
    exit 1
  fi
}

_wch_verify_archive_sha256() {
  local file="$1"
  if [[ "$SKIP_ARCHIVE_SHA256" -eq 1 ]]; then
    echo "==> Skipping SHA256 verification: $(basename "$file")"
    return 0
  fi
  if ! command -v sha256sum >/dev/null 2>&1; then
    echo "Error: sha256sum is required" >&2
    exit 1
  fi
  local expect got
  expect="${WCH_QEMU_SHA256,,}"
  got=$(sha256sum "$file" | awk '{print $1}')
  got="${got,,}"
  if [[ "$got" != "$expect" ]]; then
    echo "Error: SHA256 mismatch: $(basename "$file")" >&2
    echo "  Expected: $expect" >&2
    echo "  Actual:   $got" >&2
    exit 1
  fi
  echo "==> SHA256 verification passed: $(basename "$file")"
}

# --- Argument parsing ---

PREFIX="${PREFIX_DEFAULT}"
DOWNLOADS_DIR="${DOWNLOADS_DEFAULT}"
BUILD_TOP="${BUILD_TOP_DEFAULT}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)
      PREFIX=$2
      shift 2
      ;;
    --downloads-dir)
      DOWNLOADS_DIR=$2
      shift 2
      ;;
    --build-dir)
      BUILD_TOP=$2
      shift 2
      ;;
    -j)
      JOBS=$2
      shift 2
      ;;
    -j[0-9]*)
      JOBS=${1#-j}
      shift
      ;;
    --no-install-deps)
      INSTALL_DEB_DEPS=0
      shift
      ;;
    --skip-archive-sha256)
      SKIP_ARCHIVE_SHA256=1
      shift
      ;;
    --force-rebuild)
      FORCE_REBUILD=1
      CLEAN_BUILD=1
      shift
      ;;
    --clean)
      CLEAN_BUILD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# WCH_SKIP_ARCHIVE_SHA256 environment variable also triggers skipping
if [[ "${WCH_SKIP_ARCHIVE_SHA256:-0}" == "1" ]]; then
  SKIP_ARCHIVE_SHA256=1
fi

# --- Main workflow ---

mkdir -p "${DOWNLOADS_DIR}" "${BUILD_TOP}"

arc_name=$(basename "${WCH_QEMU_URL%%\?*}")
archive_path="${DOWNLOADS_DIR}/${arc_name}"
src_dir="${BUILD_TOP}/qemu-${WCH_QEMU_VERSION}"
obj_dir="${BUILD_TOP}/qemu-${WCH_QEMU_VERSION}-obj"

# 1. Download
if [[ ! -f "$archive_path" ]]; then
  echo "==> Downloading: ${WCH_QEMU_URL}"
  _wch_fetch_url "${WCH_QEMU_URL}" "$archive_path"
else
  echo "==> Archive already exists: ${arc_name}"
fi
_wch_verify_archive_sha256 "$archive_path"

# 2. Extract
if [[ "$FORCE_REBUILD" -eq 1 ]]; then
  echo "==> --force-rebuild: Removing old source directory ${src_dir}"
  rm -rf "${src_dir}"
fi
if [[ ! -f "${src_dir}/configure" ]]; then
  echo "==> Extracting to ${src_dir}"
  rm -rf "${src_dir}"
  tar -xJf "$archive_path" -C "${BUILD_TOP}"
else
  echo "==> Source directory already exists: ${src_dir}"
fi

# 3. Overlay CH32 machine model sources (hw/riscv/ch32-*.c / ch32-*.h)
# Only copy when content differs to avoid updating mtime and triggering unnecessary ninja rebuilds.
_install_if_changed() {
  local src="$1" dst="$2"
  if [[ ! -f "$dst" ]] || ! cmp -s "$src" "$dst"; then
    cp -f "$src" "$dst"
    return 0  # changed
  fi
  return 1  # unchanged
}

_overlay_hw="${REPO_ROOT}/qemu-overlay/${WCH_QEMU_VERSION}/hw/riscv"
_overlay_changed=0
if [[ -d "${_overlay_hw}" ]]; then
  shopt -s nullglob
  _ch32_files=("${_overlay_hw}"/ch32*.c "${_overlay_hw}"/ch32*.h)
  if ((${#_ch32_files[@]})); then
    _changed=0
    for _f in "${_ch32_files[@]}"; do
      if _install_if_changed "${_f}" "${src_dir}/hw/riscv/$(basename "${_f}")"; then
        _changed=$((_changed + 1))
      fi
    done
    if (( _changed > 0 )); then
      echo "==> Installed CH32 machine model overlay files (hw/riscv/ch32-*.c / ch32-*.h): ${_changed} file(s) changed"
      _overlay_changed=1
    else
      echo "==> CH32 machine model overlay files unchanged, skipping copy"
    fi
    # Cleanup: remove ch32*.c/.h files in src_dir that no longer exist in the overlay
    # (after renames/splits/deletions, stale files may cause duplicate definitions or stale build.ninja refs)
    _removed=0
    shopt -s nullglob
    for _f in "${src_dir}/hw/riscv"/ch32*.c "${src_dir}/hw/riscv"/ch32*.h; do
      if [[ ! -e "${_overlay_hw}/$(basename "${_f}")" ]]; then
        rm -f "${_f}"
        _removed=$((_removed + 1))
      fi
    done
    shopt -u nullglob
    if (( _removed > 0 )); then
      echo "==> Cleaned up ${_removed} stale CH32 file(s) no longer in overlay"
      _overlay_changed=1
    fi
  fi
  shopt -u nullglob
fi

# Overlay QingKe/XW/HPE target/riscv files (recursive, including subdirectories such as insn_trans/)
_overlay_tgt="${REPO_ROOT}/qemu-overlay/${WCH_QEMU_VERSION}/target/riscv"
if [[ -d "${_overlay_tgt}" ]]; then
  _changed=0
  while IFS= read -r -d '' _f; do
    _rel="${_f#${_overlay_tgt}/}"
    _dst="${src_dir}/target/riscv/${_rel}"
    mkdir -p "$(dirname "${_dst}")"
    if _install_if_changed "${_f}" "${_dst}"; then
      _changed=$((_changed + 1))
    fi
  done < <(find "${_overlay_tgt}" -type f -print0)
  if (( _changed > 0 )); then
    echo "==> Installed QingKe/XW/HPE target/riscv overlay files (including subdirs): ${_changed} file(s) changed"
    _overlay_changed=1
  else
    echo "==> target/riscv overlay files unchanged, skipping copy"
  fi
fi

# 4. Apply patches/qemu/*.patch (sorted by filename)
# A stamp file prevents duplicate patching; reset after each overlay install
_patch_stamp="${src_dir}/.wch-patches-applied"
if [[ -d "${REPO_ROOT}/patches/qemu" ]]; then
  shopt -s nullglob
  _patch_files=("${REPO_ROOT}/patches/qemu"/*.patch)
  readarray -t _patch_list < <(printf '%s\n' "${_patch_files[@]}" | LC_ALL=C sort)
  for _p in "${_patch_list[@]}"; do
    [[ -f "${_p}" ]] || continue
    echo "==> Applying patch: $(basename "${_p}")"
    if ! patch -d "${src_dir}" --forward --strip=1 -r- < "${_p}"; then
      echo "Note: If the patch was skipped, it was likely already applied; otherwise please check for conflicts." >&2
    fi
  done
  shopt -u nullglob
fi

# 5. Install build dependencies (Debian/Ubuntu)
_wch_ensure_debian_build_deps

# 6. Configure
# - Incremental by default: skip configure if obj_dir/build.ninja exists
# - --clean / --force-rebuild forces a fresh configure
if [[ "$CLEAN_BUILD" -eq 1 ]]; then
  echo "==> --clean: Removing old build directory ${obj_dir}"
  rm -rf "${obj_dir}"
fi
mkdir -p "${obj_dir}"

if [[ ! -f "${obj_dir}/build.ninja" ]]; then
  echo "==> configure (target: riscv32-softmmu, install prefix: ${PREFIX})"
  (
    cd "${obj_dir}"
    "${src_dir}/configure" \
      --prefix="${PREFIX}" \
      --target-list=riscv32-softmmu \
      --disable-docs \
      --disable-gtk \
      --disable-sdl \
      --disable-vnc \
      --disable-xen \
      --disable-bsd-user \
      --disable-linux-user \
      --disable-install-blobs \
      --disable-werror
  )
else
  echo "==> Existing build.ninja found, skipping configure (incremental build; use --clean to reconfigure)"
fi

# 7. Build
echo "==> make -j${JOBS}"
make -C "${obj_dir}" -j"${JOBS}"

# 8. Install
echo "==> make install"
make -C "${obj_dir}" install

echo ""
echo "==> Done. Executable installed at:"
echo "    ${PREFIX}/bin/qemu-system-riscv32"
echo ""
echo "    Add the following directory to your PATH (or use the absolute path):"
echo "    export PATH=\"${PREFIX}/bin:\$PATH\""
echo ""
echo "    Quick verification:"
echo "    qemu-system-riscv32 --version"
echo ""
echo "    Run CH32V317 emulation (firmware must be built first):"
echo "    ${REPO_ROOT}/scripts/run-qemu-ch32v317.sh"
echo "    ${REPO_ROOT}/scripts/run-qemu-merged-flash.sh"
