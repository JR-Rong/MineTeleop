#!/usr/bin/env bash
set -u

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [ -x "$script_dir/bin/mine-teleop" ]; then
  root="$script_dir"
elif [ -x "$script_dir/../bin/mine-teleop" ]; then
  root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
else
  root="${MINE_TELEOP_HOME:-$PWD}"
fi

config="${MINE_TELEOP_CONFIG:-$root/etc/vehicle-agent.yaml}"
can_iface="${MINE_TELEOP_CAN_IFACE:-can1}"
camera_devices="${MINE_TELEOP_CAMERAS:-/dev/video0 /dev/video2}"
render_device="${MINE_TELEOP_VAAPI_DEVICE:-/dev/dri/renderD128}"
feedback_attempts="${MINE_TELEOP_FEEDBACK_ATTEMPTS:-5}"
logdir="${MINE_TELEOP_SMOKE_LOGDIR:-$root/data/manual-smoke-$(date +%Y%m%d-%H%M%S)}"

mkdir -p "$logdir" "$root/logs" "$root/data/uploader"

failures=0
warnings=0

log() {
  printf '%s\n' "$*"
}

mark_fail() {
  failures=$((failures + 1))
  log "FAIL: $*"
}

mark_warn() {
  warnings=$((warnings + 1))
  log "WARN: $*"
}

run_required() {
  name="$1"
  shift
  stdout="$logdir/$name.stdout.log"
  stderr="$logdir/$name.stderr.log"
  log "== $name =="
  "$@" >"$stdout" 2>"$stderr"
  status=$?
  cat "$stdout"
  if [ "$status" -ne 0 ]; then
    mark_fail "$name exited $status; stderr: $stderr"
  else
    log "OK: $name"
  fi
  return 0
}

run_optional() {
  name="$1"
  shift
  stdout="$logdir/$name.stdout.log"
  stderr="$logdir/$name.stderr.log"
  log "== $name =="
  "$@" >"$stdout" 2>"$stderr"
  status=$?
  cat "$stdout"
  if [ "$status" -ne 0 ]; then
    mark_warn "$name exited $status; stderr: $stderr"
  else
    log "OK: $name"
  fi
  return 0
}

log "MINE_TELEOP_ROOT=$root"
log "MINE_TELEOP_CONFIG=$config"
log "SMOKE_LOG_DIR=$logdir"

if [ ! -x "$root/bin/mine-teleop" ]; then
  mark_fail "missing executable: $root/bin/mine-teleop"
fi
if [ ! -f "$config" ]; then
  mark_fail "missing config: $config"
fi

log "== no systemd service =="
if command -v systemctl >/dev/null 2>&1; then
  systemctl list-unit-files 'mine-teleop*' --no-pager >"$logdir/systemd-unit-files.log" 2>"$logdir/systemd-unit-files.err"
  cat "$logdir/systemd-unit-files.log"
  if grep -Eq '^mine-teleop[^[:space:]]*[[:space:]]' "$logdir/systemd-unit-files.log"; then
    mark_fail "mine-teleop systemd unit is installed"
  else
    log "OK: no mine-teleop systemd unit files"
  fi
else
  mark_warn "systemctl not available; service check skipped"
fi

log "== no lingering process =="
if command -v pgrep >/dev/null 2>&1; then
  pgrep -af "$root/bin/(mine-teleop|mine-teleop.real|ffmpeg|ffmpeg.real|ffprobe|ffprobe.real|vainfo|vainfo.real)" \
    >"$logdir/lingering-processes.log" 2>/dev/null
  if [ -s "$logdir/lingering-processes.log" ]; then
    cat "$logdir/lingering-processes.log"
    mark_warn "processes from $root/bin are already running"
  else
    log "OK: no running process from $root/bin"
  fi
else
  mark_warn "pgrep not available; process check skipped"
fi

run_required mine-teleop-list "$root/bin/mine-teleop" --list

run_required ffmpeg-hwaccels "$root/bin/ffmpeg" -hide_banner -hwaccels
if ! grep -q '^vaapi$' "$logdir/ffmpeg-hwaccels.stdout.log" "$logdir/ffmpeg-hwaccels.stderr.log"; then
  mark_fail "bundled ffmpeg did not list vaapi"
fi

run_required vainfo-drm "$root/bin/vainfo" --display drm --device "$render_device"
if ! grep -Eq 'iHD_drv_video|VAEntrypointEncSlice' "$logdir/vainfo-drm.stdout.log" "$logdir/vainfo-drm.stderr.log"; then
  mark_fail "vainfo did not show bundled Intel VAAPI encode support"
fi

vaapi_out="$logdir/vaapi-smoke.mp4"
run_required vaapi-encode "$root/bin/ffmpeg" -hide_banner -y \
  -f lavfi -i testsrc2=size=1280x720:rate=30 -t 1 \
  -vf format=nv12,hwupload -vaapi_device "$render_device" \
  -c:v h264_vaapi "$vaapi_out"
run_required vaapi-ffprobe "$root/bin/ffprobe" -v error -select_streams v:0 \
  -show_entries stream=codec_name,width,height,avg_frame_rate \
  -of default=nw=1 "$vaapi_out"

for dev in $camera_devices; do
  safe_name="camera-${dev#/dev/}"
  run_required "$safe_name" "$root/bin/ffmpeg" -hide_banner -y \
    -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 \
    -t 1 -i "$dev" -f null -
done

run_required "can-${can_iface}-details" ip -details link show "$can_iface"
if ! grep -Eq "state UP|LOWER_UP" "$logdir/can-${can_iface}-details.stdout.log"; then
  mark_fail "$can_iface is not UP/LOWER_UP"
fi
run_required "can-${can_iface}-statistics" ip -statistics link show "$can_iface"

run_required vehicle-preflight "$root/bin/mine-teleop" vehicle-agent --config "$config" --preflight
if ! grep -q '"ready": true' "$logdir/vehicle-preflight.stdout.log"; then
  mark_fail "vehicle preflight did not report ready=true"
fi

run_required adapter-status "$root/bin/mine-teleop" vehicle-agent --config "$config" --adapter-status
if ! grep -q '"ready": true' "$logdir/adapter-status.stdout.log"; then
  mark_fail "adapter status did not report ready=true"
fi
if ! grep -q '"healthy": true' "$logdir/adapter-status.stdout.log"; then
  mark_fail "adapter status did not report healthy=true"
fi

feedback_event=0
feedback_received=0
for attempt in $(seq 1 "$feedback_attempts"); do
  run_optional "adapter-feedback-$attempt" "$root/bin/mine-teleop" vehicle-agent --config "$config" --adapter-status --poll-feedback
  if grep -q '"event": "vehicle_adapter_feedback_poll"' "$logdir/adapter-feedback-$attempt.stdout.log"; then
    feedback_event=1
    if grep -q '"received": true' "$logdir/adapter-feedback-$attempt.stdout.log"; then
      feedback_received=1
      log "OK: adapter feedback received on attempt $attempt"
      break
    fi
  fi
  sleep 0.5
done
if [ "$feedback_event" -eq 0 ]; then
  mark_warn "adapter feedback event was not emitted"
elif [ "$feedback_received" -eq 0 ]; then
  mark_warn "adapter opened but did not receive a decoded feedback frame after $feedback_attempts attempts"
fi

run_required driver-console-once "$root/bin/mine-teleop" driver-console \
  --config "$root/configs/driver-console.dev.yaml" \
  --operation-log "$root/logs/driver-console-smoke.jsonl"

run_required vehicle-uploader-once "$root/bin/mine-teleop" vehicle-uploader \
  --service-mode --process-once --config "$config" \
  --work-dir "$root/data/uploader" --json

log "== signaling-loopback-health =="
port_file="$logdir/signaling.port"
server_log="$logdir/signaling-server.stdout.log"
server_err="$logdir/signaling-server.stderr.log"
"$root/bin/mine-teleop" signaling-server --serve --host 127.0.0.1 --port 0 \
  --port-file "$port_file" --vehicle-config "$config" \
  --audit-log "$logdir/signaling-audit.jsonl" >"$server_log" 2>"$server_err" &
server_pid=$!
for _ in $(seq 1 50); do
  [ -s "$port_file" ] && break
  sleep 0.1
done
if [ -s "$port_file" ]; then
  port="$(cat "$port_file")"
  if command -v curl >/dev/null 2>&1; then
    curl -fsS "http://127.0.0.1:$port/health" >"$logdir/signaling-health.json" 2>"$logdir/signaling-curl.err"
    curl_status=$?
    cat "$logdir/signaling-health.json"
    printf '\n'
    if [ "$curl_status" -ne 0 ]; then
      mark_fail "signaling health curl exited $curl_status"
    elif ! grep -q '"status": "ok"' "$logdir/signaling-health.json"; then
      mark_fail "signaling health response was not ok"
    else
      log "OK: signaling loopback health"
    fi
  else
    mark_warn "curl not available; signaling health skipped"
  fi
else
  mark_fail "signaling server did not write a port file"
fi
kill "$server_pid" >/dev/null 2>&1 || true
wait "$server_pid" >/dev/null 2>&1 || true

log "== final no lingering process =="
pgrep -af "$root/bin/(mine-teleop|mine-teleop.real|ffmpeg|ffmpeg.real|ffprobe|ffprobe.real|vainfo|vainfo.real)" \
  >"$logdir/final-lingering-processes.log" 2>/dev/null || true
if [ -s "$logdir/final-lingering-processes.log" ]; then
  cat "$logdir/final-lingering-processes.log"
  mark_warn "processes from $root/bin are still running"
else
  log "OK: no running process from $root/bin"
fi

log "SMOKE_LOG_DIR=$logdir"
log "SMOKE_FAILURES=$failures"
log "SMOKE_WARNINGS=$warnings"
if [ "$failures" -eq 0 ]; then
  log "SMOKE_RESULT=PASS"
  exit 0
fi
log "SMOKE_RESULT=FAIL"
exit 1
