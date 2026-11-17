#!/bin/bash
# The ESP32 web page is a C string. One bad escape (a "\n" that C turns into a real line break
# inside a JS string) kills the whole script: the page then sits at "link: ?" and no button works.
# This builds the page exactly as the firmware serves it and syntax-checks its script.
set -eu
cd "$(dirname "$0")/.."
f=esp32/main/web.c
out=$(mktemp -d); trap 'rm -rf "$out"' EXIT
command -v node >/dev/null || { echo "SKIP web page script check: node not installed"; exit 0; }
s=$(grep -n 'PAGE\[\] *=' $f | head -1 | cut -d: -f1)
e=$(awk -v s="$s" 'NR>s && /";$/ {print NR; exit}' $f)
{ echo '#include <stdio.h>'; sed -n "${s},${e}p" $f | sed 's/^static const char PAGE/const char PAGE/'; echo 'int main(void){fputs(PAGE,stdout);return 0;}'; } > "$out/p.c"
gcc -w "$out/p.c" -o "$out/p" && "$out/p" > "$out/page.html"
python3 -I -c "
import re,sys
h=open(sys.argv[1]).read()
m=re.search(r'<script>(.*)</script>',h,re.S)
open(sys.argv[2],'w').write(m.group(1))" "$out/page.html" "$out/s.js"
if node --check "$out/s.js" 2> "$out/err"; then echo "PASS web page script: $(wc -c < "$out/s.js") bytes parse"; else echo "FAIL web page script does not parse"; head -5 "$out/err"; exit 1; fi
