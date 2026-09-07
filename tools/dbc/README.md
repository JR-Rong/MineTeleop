# Offline DBC tooling

`check_jyr010_dbc_contract.cjs` uses only the Node standard library. It reads
the checked-in manifest, DBC, XLS hash, and golden vectors; it does not
download a generator, modify a source DBC, or emit runtime code.

Use it directly during focused work:

```bash
node tools/dbc/check_jyr010_dbc_contract.cjs
```

CTest registers the same command as `mine-teleop-dbc-codec-reference-tests`.
See [the CAN contract](../../protocol/can/README.md) for coverage and the
vehicle-acceptance boundary.
