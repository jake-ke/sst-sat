#!/bin/bash
# Build ablation configs from tools/configs.tsv into builds/<name>/.
#
#   tools/build_configs.sh                  # build every config in the manifest
#   tools/build_configs.sh baseline min     # build only the named configs
#   JOBS=16 tools/build_configs.sh          # override parallel compile jobs
#
# Each config is built FRESH (rm -rf builds/<name>) because src/Makefile has no
# header-dependency tracking -- reusing a dir after changing -D flags would keep
# stale objects.  A BUILD_INFO stamp (commit, flags, date) is written per dir so
# you can always tell which build is which.  Select a build at run time with the
# run scripts' --lib-dir builds/<name>.
set -euo pipefail

cd "$(dirname "$0")/.."
REPO=$PWD
TSV=tools/configs.tsv
JOBS=${JOBS:-8}
# Boost location (same as the SBATCH build flow); override by exporting BOOST_ROOT.
export BOOST_ROOT=${BOOST_ROOT:-/opt/spack/opt/spack/linux-ubuntu22.04-x86_64_v3/gcc-12.1.0/boost-1.82.0-bcbtzug7evlvf6xeisklxnl7ttliqgbl}

[[ -f "$TSV" ]] || { echo "manifest not found: $TSV"; exit 1; }

want=("$@")
in_want() {
    [[ ${#want[@]} -eq 0 ]] && return 0
    local w; for w in "${want[@]}"; do [[ "$w" == "$1" ]] && return 0; done
    return 1
}

commit=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
git diff --quiet 2>/dev/null && dirty=clean || dirty=dirty
built=0; skipped=0

while IFS='|' read -r name heap defines runflags; do
    # strip surrounding whitespace on each field
    name=$(echo "$name" | xargs 2>/dev/null || true)
    heap=$(echo "$heap" | xargs 2>/dev/null || true)
    defines=$(echo "$defines" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    runflags=$(echo "$runflags" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
    [[ -z "$name" || "$name" == \#* ]] && continue
    in_want "$name" || { skipped=$((skipped+1)); continue; }

    bdir="builds/$name"
    heap_arg=""
    [[ "$heap" == classic ]] && heap_arg="HEAP_IMPL=classic"

    echo "=== building '$name'  (heap=$heap, defines=${defines:-<defaults>}) ==="
    rm -rf "$bdir"
    make -C src -j"$JOBS" BUILD_DIR="../$bdir" $heap_arg CONFIG_DEFINES="$defines"

    {
        echo "name=$name"
        echo "commit=$commit ($dirty)"
        echo "date=$(date '+%Y-%m-%d %H:%M:%S')"
        echo "heap=$heap"
        echo "config_defines=${defines:-<structs.h defaults>}"
        echo "run_flags=$runflags"
    } > "$bdir/BUILD_INFO"
    echo "    -> $bdir/libsatsolver.so   (run flags: ${runflags:-none})"
    built=$((built+1))
done < "$TSV"

echo "built $built config(s); skipped $skipped."
[[ $built -eq 0 ]] && { echo "nothing built -- check names against $TSV"; exit 1; }
exit 0
