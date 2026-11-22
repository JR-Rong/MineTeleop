# Isolated recording attempt (PR24 S3)

The realtime encoder/parser still feeds one encoded tee. The send queue, capture
PTS freshness gate, codec choice and GOP settings retain the S2 behavior. The
recording branch now ends at a synchronous appsink tap rather than a disk queue
and splitmuxsink in the vehicle pipeline. `enable-last-sample=false` avoids the
sink's extra retained encoded-buffer cache. Each callback pulls and releases the
sample, copies the entire access unit (AU) into a private preallocated arena and
returns GST_FLOW_OK, including after a recording fault. It performs no file IO,
status JSON allocation, log locking, socket wait or worker join.

The copy is bounded and has a CPU cost. GstBuffer mapping can also depend on the
encoder memory type; the arena does not prove a worst-case hardware map duration.
`copy_total_us`, `copy_max_us`, `map_total_us`, `map_max_us` expose this boundary.
Software codec tests cannot certify NVENC/VAAPI memory or hardware capture timing.
No automatic bitrate/quality adaptation was added by S3.

## Finite admission and independent writer

Each lane has `max(2, fps*2)` AU slots and a 4 MiB byte arena, with at most 16
lanes (64 MiB parent payload storage). Zero-size, invalid-time-segment, oversized
or uncopyable AUs fail recording. A full slot or byte budget rejects the whole AU
and ends this recording attempt. No P/B AU is silently skipped and recording does
not resume under the same attempt claiming completeness. Completed segments are
preserved; unfinished `.mp4.partial` files receive no complete/pending sidecar.

A dedicated parent thread transfers complete framed AUs to one POSIX worker over
bounded nonblocking AF_UNIX IPC. It pops the arena only after all header/payload
bytes are sent. The worker reconstructs the actual encoder segment plus original
PTS/DTS/duration; it never labels frames with IPC arrival time. It remuxes using
appsrc -> H264/H265 parser -> splitmuxsink/filesink, without another encoder.
The worker owns all recording directories, file IO, GST EOS/NULL and metadata.
`async-finalize=false` and at most two pending fragment identities prevent an
unbounded set of concurrent fragment finalizers. Only the latest closed identity
and counters remain after closure. These are parent/IPC/pending-fragment limits,
not a claim that every third-party GST internal allocation is a 64 MiB arena.
The child's bounded appsrc queue can overshoot its byte threshold by one bounded
AU; normal mux buffering and the finite configured GOP/segment also consume RAM.

One blocked lane can end the shared recorder worker for every recording lane.
Their live video continues. Storage failure cannot guarantee continuous complete
recording with finite memory; this deliberate policy reports inactive/incomplete,
preserves completed clips and requires an explicit future attempt/session.

## Cleanup and packaging

`mine-teleop-recorder` is installed beside `mine-teleop`, with
`$ORIGIN/../lib` build/install RPATH on the supported Linux packages. The runtime
resolves an absolute sibling, never PATH or a shell. `--help` performs no GST
initialization, config load, device/CAN/server access. Internal IPC arguments are
fixed fd 3/4. Spawn setup checks every action/attribute return, moves source FDs
above those targets, gives stdio `/dev/null`, sanitizes environment and closes all
other inherited descriptors. Linux requires glibc >=2.34 atomic spawn closefrom;
unsupported platforms fail recording instead of using a racy fd-enumeration
fallback. Linux also creates original IPC endpoints with CLOEXEC atomically.
Darwin uses CLOEXEC-default spawn and explicit fcntl; creation-to-fcntl can race
an unrelated external exec and is not claimed to be globally atomic.

Recorder-only finalization has an 8-second parent budget and a 5-second worker
EOS budget. Terminal snapshots share a 500 ms delivery budget. Failure initiates
TERM, KILL after 100 ms, and bounded WNOHANG reap for 300 ms. A kernel-blocked task
may still remain after KILL: its PID is retained as `cleanup_pending`, and another
worker cannot spawn until it is reaped. No detached hanging waiter is created.
These numbers do not alter any control/video safety threshold. Existing unrelated
parent log/file IO is outside this recording-isolation guarantee.

## Honest status and integrity evidence

Every attempt emits `vehicle_recording_attempt_started` with exact vehicle,
session, attempt ID, timestamp, expected camera IDs and summary_persisted=false.
`vehicle_media_status.status.lanes[].recording` includes configured intent,
active/state, sticky incomplete, issue/next_action, accepted AU count, worker
appsrc input count, finalized/unfinished segments, EOS, cleanup status and worker
reported time. Parent polls do not make old worker progress fresh. A successful
exit alone cannot prove completion: every expected lane must have exact accepted
coverage, true final EOS, zero unfinished fragments and at least one truly closed
segment. Missing terminal evidence uses `recording_status_unconfirmed`; it remains
conservatively incomplete and cannot display a complete session.

The local `root/<vehicle>/<session>/recording-session.status` JSON is <=64 KiB,
kind=recording_session_summary, upload_state=not_applicable. Its cameras array
contains only expected recording lanes. It carries exact scope/attempt, summary
revision, real reported/completed wall time, final EOS, attempt_complete and
sticky incomplete. Current completeness_scope=current_runtime_known_attempts;
session_history_known=false. Real EOS proves this attempt, never earlier session
coverage. The same runtime retains prior incomplete evidence across codec fallback
and reconnect for its current SID; a new worker merges same-SID persistent negative
evidence. Unknown prior history after process restart does not become full-session
complete. No unbounded cross-session history map is kept. If persistence fails,
negative live status/latest-attempt logs fence an older healthy summary; export
must report unknown/incomplete rather than infer completion from an MP4 existing.

Only actual splitmux fragment-closed evidence permits rename from `.mp4.partial`
and a new complete segment sidecar. New-schema sidecars require kind=recording_segment,
recording_status=complete, finalized=true, incomplete=false and upload_state=pending.
They initially have hash_state=pending and
integrity_scope=fragment_closed_before_archive_digest. SHA is deferred to the
independent uploader's verified atomic copy, which adds actual video_sha256,
hash_state=verified and fragment_closed_and_archive_digest_verified. Open/bad/new
summary metadata cannot default to pending or block an eligible closed segment.
Pre-S3 sidecars lacking both schema_version/kind retain explicit legacy behavior.
The uploader is not called by the media pump.

Old started_at/ended_at were both generated at stop and did not prove capture UTC.
New sidecars retain identity, codec/encoder, video filename, size and upload/hash
semantics, and mark encoded_stream_running_time_not_sensor_utc. They omit those
misleading UTC fields; worker wall-clock times describe artifact/status actions,
not sensor acquisition. Accurate UTC recording metadata requires separate source
clock evidence. S3 does not change clock thresholds to fill that gap.

## Offline tests and evidence boundaries

- `mine-teleop-recording-transport-tests`: actual production POSIX parent plus
  controlled spawned IPC worker, whole-AU slot/byte/wrap/concurrency, partial IPC,
  exact EOS, 65th terminal message at EOF, 16 lanes, sticky incomplete, malformed
  and unknown status, worker exit, unknown terminal, stalled worker, repeated
  missing-helper/FD and reconnect resource checks. No GST proof is claimed.
- `mine-teleop-recording-upload-tests`: actual upload.cpp/OpenSSL, real local files,
  closed eligibility, actual deferred SHA, open/summary exclusion, legacy and
  idempotence. Fixtures cannot establish that a video is decodable.
- `mine-teleop-recording-gstreamer-tests`: actual local videotestsrc -> x264/x265
  parser/tee -> production send queue/freshness gate/decoder and record tap ->
  production worker/splitmux/filesink. Checks healthy EOS/all-AU decoded MP4s,
  multi-lane, actual slow write, slow finalize, filesink `/dev/full`, worker death,
  live decode continuity and repeated cleanup. Hooks live only in the test binary;
  shipped worker/config exposes none. H265 fixture uses a closed GOP; it does not
  certify any hardware encoder GOP behavior. No camera, real server or CAN used.
- `mine-teleop-recorder-help`: installed worker contract smoke test.

At the S3 host checkpoint, pure parent and uploader suites actually passed with
-Werror; parent also passed ASan/UBSan. The macOS host lacks GStreamer development
packages. The new worker/GST test sources have not yet compiled/run there, and
Linux source transfer remains separately authorized. Registration/source review
is not a GST result. Hardware mapping, real 20 FPS capture, field transport,
packaged helper execution and decoder/recording performance require their own
actual evidence. This S3 delta does not change S2's frozen result or hide its
original shared-tee recording backpressure gap.
