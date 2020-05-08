#!/usr/bin/env bash
set -e

FILE_REGEXP='\.(cc|cpp|c|h|hpp)$'
exec clang-format -i $(git ls-files | grep -E "$FILE_REGEXP")
