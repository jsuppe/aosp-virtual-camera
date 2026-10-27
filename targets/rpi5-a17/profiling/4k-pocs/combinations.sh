#!/system/bin/sh
# Only the three positive independent candidates from the initial matrix.
set -eu
case_script=/data/local/tmp/vcam-4k-pocs/run-case.sh
sh "$case_script" 1010 30 true true  false false false
sh "$case_script" 1011 30 true false true  false false
sh "$case_script" 1012 30 true true  true  false false
sh "$case_script" 1013 60 true true  true  false false
# Same rendering, extra advertised diagnostic endpoints only (second APK).
sh "$case_script" 1014 40 true true true false false
sh "$case_script" 1015 45 true true true false false
sh "$case_script" 1016 50 true true true false false
sh "$case_script" 1017 30 true true true false false
# Omit HAL clear to assess whether the app-only pair can ship independently.
sh "$case_script" 1018 30 false true true false false
# Revisit cached-clock interaction after missing clock glyphs in final inspection.
sh "$case_script" 1019 30 false true true true false
sh "$case_script" 1020 60 true true true true false
