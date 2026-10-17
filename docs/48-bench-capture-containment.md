# Explicit channels 6/7 probe containment (S3.1)

Production recording/control code is frozen at S3. This is a later opt-in bench
utility delta. It does not authorize physical capture or imply it occurred.
The bench program and its Linux/GST build remain unverified until actual local
compilation and explicit execution. User approval permits test binary/script/config
upload and local builds; compiling on the bench is prohibited.

The actual probe now requires a new absolute `--evidence-dir` (existing parent,
new leaf directory). --help/--validate-only do not open devices or create evidence.
The existing copied-config guards still require control=false, mock, exactly
explicit CCG2 channel6/7 and capture/output requests20. No runtime/HTTP/CAN peer is
created. A config path alone cannot prove it is a copy; the operator retains the
original/copy hashes and uses the dedicated copy. The program records only the
config SHA, binary SHA, device identity and measurements, not credentials/config.

Before fork or any source S_FMT/S_PARM, the supervisor opens both nodes, takes
exclusive advisory flock, reads VIDEO_CAPTURE G_FMT/G_PARM on each original FD,
requires valid nonzero rational cadence, verifies distinct major/minor and exact
resolved node/sysfs inode/QUERYCAP driver/card/bus/version/capabilities. It scans
/proc fd users for these nodes and rejects any foreign or uninspectable ownership.
A non-root account may receive EACCES inspecting root services: that precondition
fails before mutation. Run only under explicitly approved minimum inspection
privileges; do not ignore scan errors or change system permissions.
It writes raw native struct hex plus all public fields, times, identities and
hashes to a private0600 journal in a new0700 directory and fsyncs file/directory.
Both baselines must succeed and persist before mutation is permitted. Identity
and ownership are rechecked immediately before fork and ownership is monitored
at500ms while the child runs. Advisory locking and proc scans cannot provide atomic
exclusion from an unrelated program that ignores locks; channels must remain
reserved to this task. A new owner aborts this task and prevents restore writes.

The explicitly typed BenchPreopenedV4l2Fd opt-in makes CameraFrameSource lazily
F_DUPFD_CLOEXEC the supervisor FD. It references the same open file-description;
ordinary production source calls still open their usual path. Opt-in is restricted
to CCG2 channel6/7. The source closes its duplicate on every existing error/stop
path; the supervisor retains the original. A bench-only observer records actual
S_FMT/S_PARM/G_PARM rc/errno and returned public fields, including first-frame
failure. This trace lives in the normal child result; abrupt death may lose it,
so only the already durable dual baseline and post-exit reads survive that case.
It is never represented as a fully captured mutation trace after SIGKILL.

Plugin initialization occurs after fork, before child threads. The child joins
both capture threads and destroys each source/local pipeline before returning its
bounded result. A supervisor stop signal, foreign owner,40s elapsed limit or child
failure terminates only this owned child; TERM escalates to KILL after200ms with
WNOHANG reap bounded to2s. Only waitpid proof of the exact child permits restoration. Large result storage
is reserved before fork; the child lifecycle lives in the outer supervisor and a
scoped finally attempts restoration after ordinary post-dispatch exceptions.
A missing/unreaped child becomes cleanup_pending and blocks restore, independent
of capture success. Linux PDEATHSIG stops an orphan child when the supervisor dies,
but cannot restore the dead parent's settings. No unknown external process is
killed. All child threads/mappings/fd duplicates are gone after a verified reap;
abnormal exit is not falsely described as successful C++ destructor cleanup.

Restoration independently checks original identity and foreign ownership, observes
current G state, records STREAMOFF and zero-buffer REQBUFS results, then restores
S_FMT followed by S_PARM on the original same handle. EINVAL from STREAMOFF may
mean the already-closed source was idle; it is recorded, never relabeled successful.
Other STREAMOFF errors or failed buffer release prohibit S restore. Actual setters
and G readbacks must succeed. Every public layout/color field and capturemode,
readbuffers,extendedmode,capability are compared; frame period uses exact rational
equivalence (1/30 equals2/60;0/0 is invalid). Original FD close rc is recorded.
A fresh O_RDONLY FD then verifies identity and G state again, with its own close
rc recorded. Any discrepancy/set/read/close failure independently fails the probe;
150 collected frames cannot mask restoration failure. No driver/service restart,
permission adjustment or existing service takeover is attempted.

The final journal/stdout separates measurement_completed, capture20_acceptance,
cleanup_result and restore_result. Exit0 means collection and documented public
state restoration, never sensor20FPS, network, hardware encoder or whole vehicle
acceptance. Real delivery, timestamp flags/sequence and raw/encode/freshsend/decode
windows still determine their separate conclusions. Unknown private controls or
sensor state affected indirectly by S_FMT are outside the two-ioctl baseline proof.
Supervisor SIGKILL, power loss, blocking driver/filesystem calls, kernel D-state
or competing-owner races cannot guarantee restoration; retain the journal, mark
restoration unconfirmed and stop further capture until reviewed.

`mine-teleop-bench-containment-tests` is an automatic portable fake-ioctl suite
that never opens any device. The model makes S_FMT change timing, verifies both
baseline failures prevent all writes, checks restoration order/equivalent cadence,
identity/owner/live-child refusal, buffer release/set failures, fresh-FD evidence
and every compared field. It does not certify Linux ioctls, driver lifetime or
physical settings. The real probe stays OFF-by-default and outside CTest.
