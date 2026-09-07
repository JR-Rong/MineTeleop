# Chassis bridge ABI migration foundation

This is a non-destructive R12 foundation for the current chassis bridge ABI.
It records repository-visible consumers and makes the existing pre-CAN ABI
gate independently inspectable. It does not remove an export, change an ABI
layout, or alter runtime loading behavior.

## Evidence boundary

The contract is fixed to the PR20 head `2aca59dbf6218659041bf1ac3daaff24fd46003e`.
Its symbol names and ABI version are grounded in these current files:

| Claim | Repository evidence |
| --- | --- |
| Public C ABI declarations | `deployments/chassis-control-bridge/mine_teleop_chassis_bridge.h:715-779` |
| Bridge reports ABI version 6 | `deployments/chassis-control-bridge/chassis_control_bridge.cpp:3155` |
| Initial ABI-version, POD-size, and required-symbol gate | `cpp/src/core.cpp:593-668` |
| Remaining adapter symbols loaded before `open_v4` | `cpp/src/core.cpp:2439-2466`, then `cpp/src/core.cpp:2504` |
| Standalone config-check ABI entry point | `cpp/apps/mine_teleop.cpp:1017-1044` |
| Package/deployment callers of config-check | `scripts/test/check_cpp_ubuntu_bundle.sh:80-103`, `scripts/deploy/deploy_vehicle_bundle.sh:379-402` |

`abi/chassis-bridge/v6/consumer-inventory.json` is intentionally an internal
inventory. It separates consumers proved by this repository from external
ChassisControl or direct C-header consumers that the repository cannot see.
An absent entry is not proof of no consumer.

## Executable preflight contract

`abi/chassis-bridge/v6/preflight-contract.json` has two profiles:

- `config_check_pre_can` mirrors the ABI handle validation used by
  `config-check`: ABI version, versioned POD-size queries, and the required
  compatibility symbols.
- `vehicle_agent_pre_can` extends that set with every additional symbol loaded
  by `ChassisControlAdapter::ensure_loaded` before `open_v4` is called.

Run the stricter profile against a staged vehicle package before activating it:

```bash
python3 tools/chassis_bridge_abi_preflight.py \
  --package-root /opt/mine-teleop/releases/CANDIDATE \
  --profile vehicle_agent_pre_can
```

For a package whose bridge has dependencies outside the system loader path,
start the command with the same `LD_LIBRARY_PATH` that the package runtime will
use. The tool only invokes declared `uint32_t` query functions; all other
entries are symbol lookups. It never calls `mine_teleop_chassis_open*`, so it
does not request CAN initialization. This guarantee is at the public C ABI
call layer: dynamic-library loading remains a trusted-artifact operation, so a
candidate must not contain an unreviewed library constructor with side effects.

The independent test demonstrates this ordering with a shared-library fixture:
its `mine_teleop_chassis_open*` functions create a marker if called. The test
checks a valid staged package, ABI-version rejection, required-symbol rejection,
and the difference between the config-check and full adapter profiles while
requiring the marker to remain absent.

```bash
bash scripts/test/check_chassis_abi_preflight.sh
```

This is a fixture/package preflight test, not vehicle or SocketCAN acceptance.

## Atomic runtime and bridge upgrade / rollback

1. Build and retain a complete immutable release pair: vehicle runtime,
   `libmine_teleop_chassis_bridge.so`, `libchassis_control.so`, configuration,
   and hashes. Do not overwrite individual bridge files beneath an active
   release.
2. Extract the candidate into a new, non-active release directory. With its
   intended loader path, run `vehicle_agent_pre_can`, then the existing
   `mine-teleop-run config-check --chassis-bridge-library ...` against the
   staged bridge. A failure leaves the active release untouched.
3. Activate the runtime and bridge together using the release-level switch
   already used by the deployment transaction. Do not pair a new runtime with
   an older bridge, or an older runtime with a new bridge, merely because one
   file passes a presence check.
4. Roll back by selecting a known complete prior release pair, not by copying
   one legacy `.so` over the new release. Re-run the same preflight and
   config-check against that prior pair before switching it active. Preserve the
   failed candidate and the prior hashes for diagnosis.

The existing deployment script already validates a staged bridge before its
release activation path; this contract makes the required symbol inventory and
no-open ordering auditable alongside that runtime check.

## Intentionally blocked destructive R12 work

External ABI consumers have not been proven absent. Until package inventories,
vehicle deployment/rollback records, and ChassisControl-owner confirmation are
available, all of the following remain blocked:

- Removing `open_v1`, `open_v2`, `open_v3`, legacy `apply_state`, or any other
  current export.
- Removing legacy POD-size queries, legacy result structures, compatibility
  fixtures, or direct-caller tests.
- Changing a function signature while retaining its current symbol name.
- Reordering, resizing, or otherwise changing public C POD layouts.
- Raising the global ABI version as part of an export-removal migration.

Any future destructive ABI change must first extend the inventory with
release/operations evidence, publish an explicit old-to-new compatibility
matrix, ship runtime and bridge atomically, and preserve a verified complete
rollback package.
