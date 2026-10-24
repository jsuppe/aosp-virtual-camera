# Matched Pi5 diagnostic experiment, not an installer

See ../../../../docs/pi5-deadline-20261023/README.md for results and limitations.
The scripts preserve the exact measured deployment paths/hashes. They assume
the diagnostic library/APK have been hash-verified and installed on the matched
Pi17 userdebug base. Do not run them on another device or substitute arbitrary
hashes to skip the guard. Existing staged APEX update failure is unresolved.

overlay.sh is root-only and restores the known factory library on completion.
run.sh expects /data/local/tmp/vcam-deadline to contain all four scripts and
impl.so; restore.sh expects the verified normal APK at the documented path.
Use sh explicitly (scripts are stored non-executable).
The trace script creates a private tracefs instance and never changes global
or preexisting tracing instances. Buffer stats are mandatory for analysis.
Normal output is restored by the EXIT handler; verify its completion and hashes.
