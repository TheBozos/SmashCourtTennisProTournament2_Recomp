#!/usr/bin/env bash
# Builds SCTPT2 on BozoEngine on a remote x86-64 Linux box over ssh and copies the programs back into build/bozo.
# Usage: scripts/remote_bozo_build.sh <SLUS_209.33> [targets...]   (default target: sct2-headless)
#   REMOTE_HOST  ssh host (default stallman)
#   REMOTE_DIR   work directory on the host, relative to its home (default sct2-remote)
#   REMOTE_NAME  name of this tree's sources and build there (default bozo)
#   REMOTE_TOOLS mise tools the build runs with (default: latest cmake and ninja, Clang 23.1.4 from conda-forge)
# The ELF is copied next to the sources on the host (game data: keep the host private, delete the tree after use).
# FFmpeg must already be on the host, in <REMOTE_DIR>/ffmpeg (include/, lib/).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="$1"
shift
TARGETS=("${@:-sct2-headless}")
HOST="${REMOTE_HOST:-stallman}"
RDIR="${REMOTE_DIR:-sct2-remote}"
NAME="${REMOTE_NAME:-bozo}"
TOOLS="${REMOTE_TOOLS:-cmake@latest ninja@latest conda:clang@23.1.4 conda:clangxx@23.1.4}"
OUT="$ROOT/build/bozo"
[ -f "$ELF" ] || { echo "usage: $0 <SLUS_209.33> [targets...]"; exit 1; }

echo "== sources to $HOST:$RDIR/$NAME/src"
ssh "$HOST" mkdir -p "$RDIR/$NAME/src" "$RDIR/$NAME/game"
rsync -aL --delete --exclude=.git --exclude='/BozoEngine/build*/' \
    "$ROOT/CMakeLists.txt" "$ROOT/cmake" "$ROOT/bozo" "$ROOT/recomp" "$ROOT/tools" "$ROOT/BozoEngine" \
    "$HOST:$RDIR/$NAME/src/"
rsync -t "$ELF" "$HOST:$RDIR/$NAME/game/SLUS_209.33"

echo "== build on $HOST"
args=$(printf '%q ' "$RDIR" "$NAME" "$TOOLS" "${TARGETS[@]}")
ssh "$HOST" "bash -s -- $args" <<'EOF'
set -euo pipefail
export MISE_EXPERIMENTAL=1
ffmpeg=$HOME/$1/ffmpeg
work=$HOME/$1/$2
read -ra tools <<< "$3"
shift 3
flags="--no-default-config --target=$(gcc -dumpmachine) --gcc-install-dir=$(dirname "$(gcc -print-libgcc-file-name)")"
ldflags="--ld-path=$(command -v ld) -Wl,-rpath-link,$ffmpeg/lib -Wl,--allow-shlib-undefined"
compiler=()
[ -f "$work/build/CMakeCache.txt" ] || compiler=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
export PKG_CONFIG_PATH=$ffmpeg/lib/pkgconfig
MISE_JOBS=1 mise install "${tools[@]}"
cmake=(mise exec "${tools[@]}" -- cmake)
"${cmake[@]}" -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSCT2_ELF="$work/game/SLUS_209.33" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$ldflags" -DCMAKE_SKIP_RPATH=ON \
    "${compiler[@]}"
"${cmake[@]}" --build "$work/build" --target "$@" -j "$(nproc)"
EOF

echo "== programs to $OUT"
mkdir -p "$OUT"
for t in "${TARGETS[@]}"; do
    rsync -tz "$HOST:$RDIR/$NAME/build/$t" "$OUT/$t"
done
