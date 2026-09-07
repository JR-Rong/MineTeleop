# JYR010 DBC codec contract

This directory freezes an offline verification boundary for the 20260714 JYR010
ADU transmit layout. It does not generate or replace the vehicle codec.

## Inputs and deterministic check

`jyr010-dbc-codec-manifest.json` binds both supplied protocol artifacts by
SHA-256:

- `JYR010_DBC_VCU_20260714.dbc` is parsed as the bit-layout authority.
- `JYR010_通讯协议_VCU_20260714.xls` is only version-bound by hash because this
  check has no OLE/XLS parser and does not infer fields from it.

Run the reference check without network access or third-party Node packages:

```bash
node tools/dbc/check_jyr010_dbc_contract.cjs
```

It verifies each declared DBC `BO_` raw ID, DLC, signal start bit, length,
little-endian order, unsignedness, factor, offset, and physical limits. It
also verifies the transformation from the DBC extended-ID representation to
the canonical 29-bit runtime ID and the SocketCAN `CAN_EFF_FLAG` form.

The script packs `fixtures/jyr010-dbc-codec-vectors.json` independently from
exact decimal strings and compares the result against static eight-byte golden
payloads. It additionally performs an in-memory `0.1 -> 0.2` MCU torque-scale
mutation and requires the layout check to reject it. A real DBC modification
fails earlier on the source SHA-256 guard unless the manifest is deliberately
reviewed and updated.

## Coverage

The runtime vector drives the existing `ParallelController` through its normal
N/EPB/manual handshake gate to `ready`, then compares all 16 frames emitted by
one `tick()` against the independent goldens:

- MCU01-08: negative lower limit, zero, non-tie rounding samples, 640 Nm,
  838.3 Nm, and all eight channel indices.
- EPS01/EPS03: all four axis mode, angle, and speed fields; a separate
  reference-only vector also covers the DBC `-1575..1575` angle endpoints.
- EHB01/EHB02: all eight channel indices including `0`, `327.6`, and
  `409.5 bar`.
- EPB, Shake, Body, and vehicle-speed/Q fields.

The runtime command deliberately contains a nonzero vehicle-speed request.
The golden `ADU_Tx_VehSpd` payload remains `0 km/h, Q=0`, preserving the
existing application safety policy rather than turning it into a DBC-driven
speed request.

The C++ test separately confirms that the existing application rejects NaN,
infinity, and torque/pressure values outside the DBC range before they reach
the codec. The DBC is a layout source; its numeric enum ranges do not by
themselves prove what an enum value means on a vehicle.

## Update rule and limits

Do not rewrite the hashes automatically. A DBC/XLS update needs a reviewed
supplier version, an explicit layout diff, regenerated independent golden
payloads, and a passing contract run. The manifest's zeroed ranges are bits
without a named DBC signal; they are asserted as zero for deterministic output,
not claimed to be vendor-confirmed reserved-bit semantics.

This is software-only evidence. It does not validate the deployed DBC version,
SocketCAN wiring, extended-ID behavior on the actual interface, enum semantics,
VCU response, torque direction, brake pressure, or vehicle safety. Those need
an isolated CAN/VCU bench and then a controlled field acceptance procedure.
