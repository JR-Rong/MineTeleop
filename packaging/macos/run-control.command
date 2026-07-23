#!/bin/zsh
set -eu

script_dir="${0:A:h}"
export CURL_CA_BUNDLE="$script_dir/certs/cacert.pem"
exec "$script_dir/bin/mine-teleop-control" --config "$script_dir/config/driver-console.yaml" "$@"
