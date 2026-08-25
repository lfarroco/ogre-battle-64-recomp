#!/bin/bash
# Re-applies manual fixes to splat-generated asm that are lost on re-split.
#
# Overlay B (40E80.s) takes the address of .L8019EE70, a loop label inside
# overlay C (1CE040.s). splat emits it as a file-local .L label, which cannot
# be linked across object files. Export it as a global symbol instead.
set -e
cd "$(dirname "$0")/.."

python3 - <<'PY'
import re

# 1CE040.s: export the label and drop the file-local .L prefix.
p = 'asm/1CE040.s'
s = open(p).read()
s = re.sub(r'^\s*\.L8019EE70:\s*$', '.globl L8019EE70\nL8019EE70:', s, flags=re.M, count=1)
s = re.sub(r'\b\.L8019EE70\b', 'L8019EE70', s)
open(p, 'w').write(s)

# 40E80.s: reference the global symbol instead of the local .L one.
p = 'asm/40E80.s'
s = open(p).read()
s = re.sub(r'%hi\(\.L8019EE70\)', '%hi(L8019EE70)', s)
s = re.sub(r'%lo\(\.L8019EE70\)', '%lo(L8019EE70)', s)
open(p, 'w').write(s)
PY
echo "cross-overlay label fix applied"
