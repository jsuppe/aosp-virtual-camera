#!/system/bin/sh
set -eu
dir=/data/local/tmp/vcam-investigation
grep -q VCAM_INVESTIGATION_RESTORED "$dir/performance.log"
tar czf /data/local/tmp/vcam-investigation-results.tgz "$dir"/*.txt "$dir"/*.log "$dir"/*.pftrace "$dir"/qual /data/user/0/com.example.vcamproducer/files/vcam-profile/*-8[34][0-9]-*.csv /data/vendor/vcam-profile/hal-8[34][0-9]-*.csv
chmod 0644 /data/local/tmp/vcam-investigation-results.tgz
wc -c /data/local/tmp/vcam-investigation-results.tgz
sha256sum /data/local/tmp/vcam-investigation-results.tgz
