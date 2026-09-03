#!/bin/sh

SCRIPT=$(realpath "$0")
DIR=$(dirname "$SCRIPT")

cd "$DIR" || exit 1

BENCHMARK="$DIR/fix_parse_benchmark"
if [ ! -x "$BENCHMARK" ] && [ -x "$DIR/../lib/fix_parse_benchmark" ]; then
  BENCHMARK="$DIR/../lib/fix_parse_benchmark"
fi

"$BENCHMARK" --data-dictionary "$DIR/../spec/FIX42.xml" "$@"
