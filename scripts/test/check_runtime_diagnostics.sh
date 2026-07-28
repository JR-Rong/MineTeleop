#!/usr/bin/env bash
set -euo pipefail

repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
media_source="$repository_root/cpp/src/media.cpp"
media_runtime="$repository_root/cpp/src/webrtc_media.cpp"
vcu_bridge="$repository_root/deployments/chassis-control-bridge/chassis_control_bridge.cpp"
catalog="$repository_root/docs/24-vehicle-runtime-diagnostics.md"

require_text() {
  local file="$1"
  local text="$2"
  if ! grep -F --quiet "$text" "$file"; then
    printf 'runtime diagnostics contract missing: file=%s text=%s\n' "$file" "$text" >&2
    exit 1
  fi
}

camera_contract=(
  'camera media source configuration is invalid|camera_config_invalid'
  'camera is not a vendor SDK source|camera_source_type_invalid'
  'cannot create media pipe|camera_bridge_pipe_failed'
  'cannot fork media process|camera_bridge_fork_failed'
  'camera process exited|camera_bridge_exited'
  'MJPEG frame exceeded|camera_frame_too_large'
  'timed out waiting for camera frame|camera_frame_timeout'
  'timed out waiting for V4L2 frame|camera_frame_timeout'
  'media poll failed|camera_poll_failed'
  'media read failed|camera_bridge_read_failed'
  'cannot open V4L2 camera|camera_open_failed'
  'VIDIOC_QUERYCAP failed|camera_querycap_failed'
  'must support V4L2 capture and streaming|camera_node_not_capture_capable'
  'VIDIOC_S_FMT MJPEG failed|camera_mjpeg_format_rejected'
  'does not provide native MJPEG|camera_native_mjpeg_unavailable'
  'VIDIOC_S_PARM failed|camera_fps_rejected'
  'mmap buffers are unavailable|camera_mmap_buffers_unavailable'
  'VIDIOC_QUERYBUF failed|camera_query_buffer_failed'
  'mmap failed|camera_mmap_failed'
  'VIDIOC_QBUF failed|camera_queue_buffer_failed'
  'VIDIOC_STREAMON failed|camera_stream_on_failed'
  'VIDIOC_DQBUF failed|camera_dequeue_buffer_failed'
  'invalid capture buffer|camera_invalid_capture_buffer'
  'invalid MJPEG frame|camera_invalid_mjpeg_frame'
  'native JPEG encoder failed|camera_test_jpeg_encode_failed'
)

for contract in "${camera_contract[@]}"; do
  error_text="${contract%%|*}"
  issue_code="${contract#*|}"
  require_text "$media_source" "$error_text"
  require_text "$media_runtime" "$issue_code"
  require_text "$catalog" "$issue_code"
done

for text in \
  '"vehicle_camera_failed"' \
  '"camera_id", lane.camera.id' \
  '"device", lane.camera.device' \
  '"operator_action"' \
  '"safety_action"'; do
  require_text "$media_runtime" "$text"
done

for issue_code in \
  vcu_log_directory_create_failed \
  vcu_log_file_open_failed \
  vcu_log_write_failed \
  vcu_log_rotation_failed \
  socketcan_open_failed \
  socketcan_receive_failed \
  can_error_or_rtr_frame_received \
  can_rx_unrecognized_or_invalid \
  vcu_critical_feedback_timeout \
  socketcan_send_failed \
  vcu_tx_deadline_missed \
  vcu_handshake_gate_rejected \
  vcu_control_runtime_unavailable \
  vcu_control_command_invalid \
  vcu_disarm_timeout; do
  require_text "$vcu_bridge" "$issue_code"
  require_text "$catalog" "$issue_code"
done

printf 'runtime_diagnostics_contract=passed camera_mappings=%d\n' "${#camera_contract[@]}"
