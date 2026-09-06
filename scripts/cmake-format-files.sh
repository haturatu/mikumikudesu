#!/usr/bin/env bash
set -euo pipefail

git ls-files -z -- \
  ':(exclude)external/**' \
  'CMakeLists.txt' \
  '**/CMakeLists.txt' \
  '*.cmake' \
  '*.cmake.in'
