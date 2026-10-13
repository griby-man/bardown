#!/usr/bin/env bash
# cppcheck over bardown's own code (src/, not the generated code or the sdk). exits non-zero on any finding.
# the compiler's -Wall -Wextra on the same files is part of the normal build
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
cppcheck src/ --std=c++20 --language=c++ -I src \
  --inline-suppr --quiet -j8 \
  --enable=warning,style,performance,portability \
  --suppress=missingIncludeSystem --suppress=missingInclude \
  -DREX_EXPORT\(a,b\)= \
  --error-exitcode=1 --template='{file}:{line}: {severity}/{id}: {message}'
echo "cppcheck: clean"
