#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Anderson S.
# ds5ctl and ds5fw against reference outputs recorded from the original Python implementations (tests/golden_ctl.txt, tests/golden_fw.sha256)
set -eu
cd "$(dirname "$0")/.."
fail=0
while IFS='|' read -r bus args want; do
    got=$(bin/ds5ctl --dry-run --bus "$bus" trigger $args)
    [ "$got" = "$want" ] || { echo "FAIL ds5ctl $bus $args"; echo "  want: $want"; echo "  got:  $got"; fail=1; }
done < tests/golden_ctl.txt
tmp=$(mktemp -d)
cc -O2 tests/mkimage.c -o "$tmp/mkimage" && "$tmp/mkimage" "$tmp/fake.bin"
bin/ds5fw --dry-run "$tmp/fake.bin" "$tmp/out.txt" >/dev/null
[ "$(sha256sum "$tmp/out.txt" | cut -d' ' -f1)" = "$(cat tests/golden_fw.sha256)" ] || { echo "FAIL ds5fw dry run differs from the Python reference"; fail=1; }
# the product id check: an image for another model is refused before anything is written; the right one goes through
if bin/ds5fw --dry-run "$tmp/fake.bin" "$tmp/refused.txt" --pad-pid 0x0DF2 2>"$tmp/err.txt"; then echo "FAIL ds5fw accepted an image for another product id"; fail=1; fi
grep -q "refusing" "$tmp/err.txt" || { echo "FAIL ds5fw did not say why it refused"; fail=1; }
[ ! -e "$tmp/refused.txt" ] || { echo "FAIL ds5fw wrote reports for a refused image"; fail=1; }
bin/ds5fw --dry-run "$tmp/fake.bin" "$tmp/ok.txt" --pad-pid 0x0CE6 >/dev/null || { echo "FAIL ds5fw refused the matching image"; fail=1; }
rm -rf "$tmp"
[ $fail = 0 ] && echo "test_tools ok: ds5ctl ($(wc -l < tests/golden_ctl.txt) reports) and ds5fw dry run (16682 reports) match the Python references; ds5fw refuses an image for another product id"
exit $fail
