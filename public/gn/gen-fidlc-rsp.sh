#!/bin/bash
#
# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -e

OUTFILE="$1"
DEPFILE="$2"
RSPFILE="$3"
shift 3

ARGS=()
FILES=()

handle_args() {
  ARGS+=("$@")
  local arg in_files=false
  for arg in "$@"; do
    if [ "$arg" = --files ]; then
      in_files=true
    elif $in_files; then
      FILES+=("$arg")
    fi
  done
}

read_rspfile() {
  local arg
  while read arg; do
    handle_args "$arg"
  done
}

write_output() {
  local arg
  for arg in "${ARGS[@]}"; do
    echo "$arg"
  done
}

handle_args "$@"
read_rspfile < "$RSPFILE"
echo "$OUTFILE: ${FILES[*]}" > "$DEPFILE"
write_output > "$OUTFILE"
