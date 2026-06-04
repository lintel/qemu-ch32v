#!/usr/bin/env bash
# 从官方源码包构建 QEMU 10.2.2（仅 riscv32-softmmu），用于 CH32V317 整机仿真。
#
# 工作流程：
#   1. 下载/校验 qemu-<版本>.tar.xz
#   2. 解压到 build/qemu-<版本>/
#   3. 覆盖 qemu-overlay/<版本>/ 中的 CH32 整机模型源码
#   4. 应用 patches/qemu/*.patch
#   5. configure → make → make install
#
# 默认路径：
#   下载缓存：<仓库>/downloads/archives/
#   源码目录：<仓库>/build/qemu-<版本>/
#   编译目录：<仓库>/build/qemu-<版本>-obj/
#   安装前缀：<仓库>/dist/qemu-<版本>-riscv32/
#
# 本脚本位于仓库根目录；详见 docs/QEMU-CH32V317本地测试环境.md

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

# --- 版本与下载地址（可用环境变量覆盖）---
WCH_QEMU_VERSION=${WCH_QEMU_VERSION:-10.2.2}
WCH_QEMU_URL=${WCH_QEMU_URL:-https://download.qemu.org/qemu-${WCH_QEMU_VERSION}.tar.xz}
# 与 https://download.qemu.org/qemu-10.2.2.tar.xz 一致（可用 WCH_QEMU_SHA256 覆盖或 --skip-archive-sha256 跳过）
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
用法: $(basename "${BASH_SOURCE[0]}") [选项]

从官方源码包构建 WCH CH32V317 专用 QEMU（riscv32-softmmu）。

默认安装前缀: ${PREFIX_DEFAULT}

选项:
  --prefix PATH          安装目录
  --downloads-dir PATH   源码包缓存目录（默认 <仓库>/downloads/archives）
  --build-dir PATH       构建根目录（默认 <仓库>/build）
  -j N | -jN             并行编译数（默认 \$(nproc)）
  --no-install-deps      跳过 Debian/Ubuntu 上的 apt 依赖安装
  --skip-archive-sha256  不校验源码包 SHA256
  --force-rebuild        强制重新解压源码（删除旧 build 目录后重建；含 --clean 效果）
  --clean                仅删除编译目录（obj_dir）后重新 configure + 全量编译
  -h, --help

环境变量:
  WCH_QEMU_VERSION       QEMU 版本号（默认 10.2.2）
  WCH_QEMU_URL           下载地址
  WCH_QEMU_SHA256        期望 SHA256（小写十六进制）
  WCH_SKIP_ARCHIVE_SHA256=1  等同 --skip-archive-sha256
  JOBS                   并行编译数
EOF
}

# --- 工具函数 ---

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
    echo "==> Debian/Ubuntu：QEMU 构建依赖已满足"
    return 0
  fi
  echo "==> Debian/Ubuntu：将安装缺失依赖: ${missing[*]}" >&2
  if [[ "$(id -u)" -ne 0 ]] && ! command -v sudo >/dev/null 2>&1; then
    echo "错误: 非 root 且无 sudo。" >&2
    exit 1
  fi
  _wch_apt_get update -qq
  _wch_apt_get install -y --no-install-recommends "${missing[@]}"
  echo "==> 依赖安装完成"
}

_wch_fetch_url() {
  local url="$1" out="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fL --connect-timeout 60 --retry 3 --retry-delay 2 -o "$out" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$out" "$url"
  else
    echo "错误: 需要 curl 或 wget" >&2
    exit 1
  fi
}

_wch_verify_archive_sha256() {
  local file="$1"
  if [[ "$SKIP_ARCHIVE_SHA256" -eq 1 ]]; then
    echo "==> 跳过 SHA256: $(basename "$file")"
    return 0
  fi
  if ! command -v sha256sum >/dev/null 2>&1; then
    echo "错误: 需要 sha256sum" >&2
    exit 1
  fi
  local expect got
  expect=$(printf '%s' "$WCH_QEMU_SHA256" | tr '[:upper:]' '[:lower:]')
  got=$(sha256sum "$file" | awk '{print $1}')
  got=$(printf '%s' "$got" | tr '[:upper:]' '[:lower:]')
  if [[ "$got" != "$expect" ]]; then
    echo "错误: SHA256 不匹配: $(basename "$file")" >&2
    echo "  期望: $expect" >&2
    echo "  实际: $got" >&2
    exit 1
  fi
  echo "==> SHA256 校验通过: $(basename "$file")"
}

# --- 参数解析 ---

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
      echo "未知选项: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# WCH_SKIP_ARCHIVE_SHA256 环境变量也可触发跳过
if [[ "${WCH_SKIP_ARCHIVE_SHA256:-0}" == "1" ]]; then
  SKIP_ARCHIVE_SHA256=1
fi

# --- 主流程 ---

mkdir -p "${DOWNLOADS_DIR}" "${BUILD_TOP}"

arc_name=$(basename "${WCH_QEMU_URL%%\?*}")
archive_path="${DOWNLOADS_DIR}/${arc_name}"
src_dir="${BUILD_TOP}/qemu-${WCH_QEMU_VERSION}"
obj_dir="${BUILD_TOP}/qemu-${WCH_QEMU_VERSION}-obj"

# 1. 下载
if [[ ! -f "$archive_path" ]]; then
  echo "==> 下载: ${WCH_QEMU_URL}"
  _wch_fetch_url "${WCH_QEMU_URL}" "$archive_path"
else
  echo "==> 已有压缩包: ${arc_name}"
fi
_wch_verify_archive_sha256 "$archive_path"

# 2. 解压
if [[ "$FORCE_REBUILD" -eq 1 ]]; then
  echo "==> --force-rebuild: 删除旧源码目录 ${src_dir}"
  rm -rf "${src_dir}"
fi
if [[ ! -f "${src_dir}/configure" ]]; then
  echo "==> 解压到 ${src_dir}"
  rm -rf "${src_dir}"
  tar -xJf "$archive_path" -C "${BUILD_TOP}" \
    --exclude='qemu-*/roms' \
    --exclude='qemu-*/tests/lcitool/libvirt-ci'
else
  echo "==> 已有源码目录: ${src_dir}"
fi

# 3. 覆盖 CH32 整机模型源码（hw/riscv/ch32-*.c / ch32-*.h）
# 仅当内容变化时拷贝，避免更新 mtime 触发 ninja 不必要的重编。
_install_if_changed() {
  local src="$1" dst="$2"
  if [[ ! -f "$dst" ]] || ! cmp -s "$src" "$dst"; then
    cp -f "$src" "$dst"
    return 0  # 改动了
  fi
  return 1  # 未改动
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
      echo "==> 安装 CH32 整机模型覆盖文件 (hw/riscv/ch32-*.c / ch32-*.h)：${_changed} 个变化文件"
      _overlay_changed=1
    else
      echo "==> CH32 整机模型覆盖文件无改动，跳过拷贝"
    fi
    # 清理：src_dir 中存在、但已不在 overlay 中的 ch32*.c/.h 残留（文件被重命名/拆分/删除后
    # 需同步清理，否则多重定义/ meson 未引用的旧文件仍会被前一次的 build.ninja 引用）
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
      echo "==> 清理已不在 overlay 中的 CH32 残留文件：${_removed} 个"
      _overlay_changed=1
    fi
  fi
  shopt -u nullglob
fi

# 覆盖 QingKe/XW/HPE 等 target/riscv 文件（递归包含子目录，如 insn_trans/）
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
    echo "==> 安装 QingKe/XW/HPE target/riscv 覆盖文件（包含子目录）：${_changed} 个变化文件"
    _overlay_changed=1
  else
    echo "==> target/riscv 覆盖文件无改动，跳过拷贝"
  fi
fi

# 4. 应用 patches/qemu/*.patch（按文件名排序）
# 使用标记文件避免重复打补丁：每次 overlay 安装后重置标记
_patch_stamp="${src_dir}/.wch-patches-applied"
if [[ -d "${REPO_ROOT}/patches/qemu" ]]; then
  shopt -s nullglob
  _patch_files=("${REPO_ROOT}/patches/qemu"/*.patch)
  _patch_list=()
  while IFS= read -r _line; do
    [[ -n "$_line" ]] && _patch_list+=("$_line")
  done < <(printf '%s\n' "${_patch_files[@]}" | LC_ALL=C sort)
  for _p in "${_patch_list[@]}"; do
    [[ -f "${_p}" ]] || continue
    echo "==> 应用补丁: $(basename "${_p}")"
    if ! patch -d "${src_dir}" --forward --strip=1 -r- < "${_p}"; then
      echo "提示: 若提示已忽略补丁，多为重复执行；否则请检查冲突。" >&2
    fi
  done
  shopt -u nullglob
fi

# 5. 安装构建依赖（Debian/Ubuntu）
_wch_ensure_debian_build_deps

# 6. configure
# - 默认增量：若 obj_dir/build.ninja 存在则跳过 configure，直接 ninja
# - --clean / --force-rebuild 强制重新 configure
if [[ "$CLEAN_BUILD" -eq 1 ]]; then
  echo "==> --clean：删除旧编译目录 ${obj_dir}"
  rm -rf "${obj_dir}"
fi
mkdir -p "${obj_dir}"

if [[ ! -f "${obj_dir}/build.ninja" ]]; then
  echo "==> configure（目标: riscv32-softmmu，安装前缀: ${PREFIX}）"
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
      --disable-cocoa \
      --disable-guest-agent \
      --disable-werror
  )
else
  echo "==> 检测到现有 build.ninja，跳过 configure（使用增量编译；如需重配请加 --clean）"
fi

# 7. 编译
echo "==> make -j${JOBS}"
make -C "${obj_dir}" -j"${JOBS}"

# 8. 安装
echo "==> make install"
make -C "${obj_dir}" install

echo ""
echo "==> 完成。可执行文件位于："
echo "    ${PREFIX}/bin/qemu-system-riscv32"
echo ""
echo "    将下列目录加入 PATH（或直接使用绝对路径）："
echo "    export PATH=\"${PREFIX}/bin:\$PATH\""
echo ""
echo "    快速验证："
echo "    qemu-system-riscv32 --version"
echo ""
echo "    运行 CH32V317 仿真（需先构建固件）："
echo "    ${REPO_ROOT}/scripts/run-qemu-ch32v317.sh"
echo "    ${REPO_ROOT}/scripts/run-qemu-merged-flash.sh"
