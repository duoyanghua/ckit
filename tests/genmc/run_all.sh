#!/usr/bin/env bash
# Run every GenMC protocol model.  Requires the GenMC driver:
#   GENMC=/path/to/genmc bash tests/genmc/run_all.sh
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/.." && pwd)"
genmc="${GENMC:-genmc}"

if ! command -v "$genmc" >/dev/null 2>&1; then
  echo "genmc not found; set GENMC=/path/to/genmc" >&2
  exit 127
fi

shopt -s nullglob
models=("$@")
if [ ${#models[@]} -eq 0 ]; then
  models=("$root"/reclamation/genmc/*.cpp "$root"/threadpool/genmc/*.cpp)
fi

fail=0
for model in "${models[@]}"; do
  printf '\n== %s ==\n' "${model#"$root"/}"
  if ! "$genmc" -- "$model"; then
    fail=1
  fi
done

exit "$fail"
