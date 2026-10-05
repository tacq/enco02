#!/bin/sh
# Runs the pure-Dart unit tests on the plain Dart VM (JIT), one file at a time.
# `flutter test` / `dart test` need dartaotruntime, which Santa blocks on corp Macs; running a
# package:test file directly with `dart` does not. Exit code is non-zero if any file fails.
set -e
cd "$(dirname "$0")/.."
status=0
for f in test/*_test.dart; do
  echo "== $f"
  dart "$f" || status=1
done
exit $status
