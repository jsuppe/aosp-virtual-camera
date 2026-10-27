#!/system/bin/sh
set -eu
case_script=/data/local/tmp/vcam-4k-pocs/run-case.sh
# id fps HAL-clear left-SurfaceView preview-sized cached-clock render-once
sh "$case_script" 1001 30 false false false false false
sh "$case_script" 1002 30 true  false false false false
sh "$case_script" 1003 30 false true  false false false
sh "$case_script" 1004 30 false false true  false false
sh "$case_script" 1005 30 false false false true  false
sh "$case_script" 1006 30 false false false false true
sh "$case_script" 1007 30 false false false false false
