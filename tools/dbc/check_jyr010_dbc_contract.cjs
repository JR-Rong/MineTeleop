'use strict';

const assert = require('assert').strict;
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '../..');
const manifestPath = path.join(repoRoot, 'protocol', 'can', 'jyr010-dbc-codec-manifest.json');
const vectorsPath = path.join(
    repoRoot, 'protocol', 'can', 'fixtures', 'jyr010-dbc-codec-vectors.json');

function fail(message) {
  throw new Error(message);
}

function readJson(filePath) {
  return JSON.parse(fs.readFileSync(filePath, 'utf8'));
}

function sha256(filePath) {
  return crypto.createHash('sha256').update(fs.readFileSync(filePath)).digest('hex');
}

function parseDecimal(value) {
  const text = String(value);
  const match = /^([+-]?)(\d+)(?:\.(\d+))?$/.exec(text);
  if (!match) fail(`not a finite decimal literal: ${text}`);
  const denominator = 10n ** BigInt((match[3] || '').length);
  let numerator = BigInt(`${match[2]}${match[3] || ''}`);
  if (match[1] === '-') numerator = -numerator;
  return {numerator, denominator};
}

function compareRational(left, right) {
  const delta = left.numerator * right.denominator - right.numerator * left.denominator;
  return delta < 0n ? -1 : delta > 0n ? 1 : 0;
}

function subtractRational(left, right) {
  return {
    numerator: left.numerator * right.denominator - right.numerator * left.denominator,
    denominator: left.denominator * right.denominator,
  };
}

function divideRational(left, right) {
  if (right.numerator === 0n) fail('cannot divide by zero factor');
  return {
    numerator: left.numerator * right.denominator,
    denominator: left.denominator * right.numerator,
  };
}

function nearestIntegerForNonTie(value) {
  if (value.denominator === 0n) fail('zero denominator');
  const denominator = value.denominator < 0n ? -value.denominator : value.denominator;
  const numerator = value.denominator < 0n ? -value.numerator : value.numerator;
  const negative = numerator < 0n;
  const magnitude = negative ? -numerator : numerator;
  let rounded = magnitude / denominator;
  const remainder = magnitude % denominator;
  assert.notEqual(
      remainder * 2n,
      denominator,
      'half-step physical reference values require a supplier rounding rule and are intentionally unsupported');
  if (remainder * 2n > denominator) rounded += 1n;
  return negative ? -rounded : rounded;
}

function hexToBigInt(value) {
  if (typeof value !== 'string' || !/^0x[0-9a-f]+$/i.test(value)) {
    fail(`expected hexadecimal identifier, got ${String(value)}`);
  }
  return BigInt(value);
}

function normalizeHex(value) {
  return `0x${value.toString(16).toUpperCase().padStart(8, '0')}`;
}

function bitMask(length) {
  return (1n << BigInt(length)) - 1n;
}

function payloadToHex(payload) {
  let output = '';
  for (let index = 0; index < 8; index += 1) {
    output += Number((payload >> BigInt(index * 8)) & 0xFFn).toString(16).padStart(2, '0');
  }
  return output;
}

function parseDbc(text) {
  const messages = new Map();
  let current = null;
  for (const line of text.split(/\r?\n/)) {
    const messageMatch = /^BO_\s+(\d+)\s+(\S+):\s+(\d+)\s+/.exec(line);
    if (messageMatch) {
      current = {
        rawId: messageMatch[1],
        name: messageMatch[2],
        dlc: Number(messageMatch[3]),
        signals: new Map(),
      };
      messages.set(current.name, current);
      continue;
    }
    const signalMatch = /^\s+SG_\s+(\S+)\s*:\s*(\d+)\|(\d+)@(\d)([+-])\s+\(([^,]+),([^)]*)\)\s+\[([^|]+)\|([^\]]+)\]/.exec(line);
    if (signalMatch && current) {
      current.signals.set(signalMatch[1], {
        startBit: Number(signalMatch[2]),
        length: Number(signalMatch[3]),
        byteOrder: signalMatch[4],
        sign: signalMatch[5],
        factor: signalMatch[6].trim(),
        offset: signalMatch[7].trim(),
        minimum: signalMatch[8].trim(),
        maximum: signalMatch[9].trim(),
      });
    }
  }
  return messages;
}

function expandTemplate(value, index) {
  return value.split('{index02}').join(String(index).padStart(2, '0'));
}

function resolveMessages(manifest) {
  const records = [];
  for (const message of manifest.messages || []) {
    records.push({...message});
  }
  for (const family of manifest.families || []) {
    for (const instance of family.instances || []) {
      const record = {
        name: expandTemplate(family.dbc_message_name_template, instance.index),
        direction: family.direction,
        layout: family.layout,
        dbc_raw_id: instance.dbc_raw_id,
        arbitration_id: instance.arbitration_id,
        socketcan_can_id: instance.socketcan_can_id,
        dbc_signal_names: {},
      };
      for (const [key, template] of Object.entries(family.dbc_signal_name_templates || {})) {
        record.dbc_signal_names[key] = expandTemplate(template, instance.index);
      }
      records.push(record);
    }
  }
  const uniqueNames = new Set(records.map((record) => record.name));
  assert.equal(uniqueNames.size, records.length, 'manifest contains duplicate CAN message names');
  return records;
}

function assertLayout(layoutName, layout) {
  assert.equal(layout.dlc, 8, `${layoutName}: only CAN classic DLC 8 is supported here`);
  assert.equal(layout.byte_order, 'little_endian', `${layoutName}: unexpected byte order`);
  assert(Array.isArray(layout.signals) && layout.signals.length > 0,
      `${layoutName}: signals must be a nonempty array`);
  const usedBits = new Set();
  for (const signal of layout.signals) {
    assert.equal(signal.signed, false, `${layoutName}.${signal.key}: signed signals are not in this scope`);
    assert(Number.isInteger(signal.start_bit) && Number.isInteger(signal.length),
        `${layoutName}.${signal.key}: start_bit and length must be integers`);
    assert(signal.start_bit >= 0 && signal.length > 0 && signal.start_bit + signal.length <= 64,
        `${layoutName}.${signal.key}: field exceeds the 8-byte CAN payload`);
    parseDecimal(signal.factor);
    parseDecimal(signal.offset);
    parseDecimal(signal.minimum);
    parseDecimal(signal.maximum);
    for (let bit = signal.start_bit; bit < signal.start_bit + signal.length; bit += 1) {
      assert(!usedBits.has(bit), `${layoutName}: signals overlap at bit ${bit}`);
      usedBits.add(bit);
    }
  }
  for (const range of layout.zeroed_unassigned_bit_ranges || []) {
    assert(Array.isArray(range) && range.length === 2, `${layoutName}: invalid zeroed bit range`);
    const [first, last] = range;
    assert(Number.isInteger(first) && Number.isInteger(last) && first >= 0 && last < 64 && first <= last,
        `${layoutName}: invalid zeroed bit range ${range}`);
    for (let bit = first; bit <= last; bit += 1) {
      assert(!usedBits.has(bit), `${layoutName}: zeroed range overlaps named signal at bit ${bit}`);
    }
  }
}

function assertSignalMatchesDbc(message, layout, key, expectedName) {
  const signal = layout.signals.find((candidate) => candidate.key === key);
  assert(signal, `${message.name}: layout has no ${key} signal`);
  const dbcSignal = message.signals.get(expectedName);
  assert(dbcSignal, `${message.name}: DBC signal is missing: ${expectedName}`);
  assert.equal(dbcSignal.startBit, signal.start_bit, `${message.name}.${key}: DBC start bit drifted`);
  assert.equal(dbcSignal.length, signal.length, `${message.name}.${key}: DBC length drifted`);
  assert.equal(dbcSignal.byteOrder, '1', `${message.name}.${key}: DBC byte order drifted`);
  assert.equal(dbcSignal.sign, '+', `${message.name}.${key}: DBC signedness drifted`);
  for (const keyName of ['factor', 'offset', 'minimum', 'maximum']) {
    assert.equal(
        compareRational(parseDecimal(dbcSignal[keyName]), parseDecimal(signal[keyName])),
        0,
        `${message.name}.${key}: DBC ${keyName} drifted from manifest`);
  }
}

function assertMessageMatchesDbc(record, manifest, dbcMessages) {
  const layout = manifest.layouts[record.layout];
  assert(layout, `${record.name}: unknown layout ${record.layout}`);
  const dbcMessage = dbcMessages.get(record.name);
  assert(dbcMessage, `DBC message is missing: ${record.name}`);
  assert.equal(dbcMessage.rawId, record.dbc_raw_id, `${record.name}: DBC raw ID drifted`);
  assert.equal(dbcMessage.dlc, layout.dlc, `${record.name}: DBC DLC drifted`);

  const marker = hexToBigInt(manifest.id_encoding.dbc_bo_extended_marker);
  const mask = hexToBigInt(manifest.id_encoding.arbitration_id_mask);
  const socketcanFlag = hexToBigInt(manifest.id_encoding.socketcan_eff_flag);
  const rawId = BigInt(record.dbc_raw_id);
  const arbitrationId = hexToBigInt(record.arbitration_id);
  const socketcanId = hexToBigInt(record.socketcan_can_id);
  assert.equal(rawId & marker, marker, `${record.name}: DBC ID is not flagged as extended`);
  assert.equal(rawId & mask, arbitrationId, `${record.name}: DBC ID does not decode to the declared arbitration ID`);
  assert.equal(rawId, arbitrationId | marker, `${record.name}: DBC raw ID conversion drifted`);
  assert.equal(socketcanId, arbitrationId | socketcanFlag,
      `${record.name}: SocketCAN CAN_EFF_FLAG conversion drifted`);
  assert.equal(normalizeHex(socketcanId).toLowerCase(), record.socketcan_can_id.toLowerCase(),
      `${record.name}: non-canonical SocketCAN identifier spelling`);

  const namedKeys = Object.keys(record.dbc_signal_names || {}).sort();
  const layoutKeys = layout.signals.map((signal) => signal.key).sort();
  assert.deepEqual(namedKeys, layoutKeys, `${record.name}: manifest signal-to-DBC mapping is incomplete`);
  for (const [key, dbcSignalName] of Object.entries(record.dbc_signal_names)) {
    assertSignalMatchesDbc(dbcMessage, layout, key, dbcSignalName);
  }
}

function encodeSignal(signal, valueText) {
  const value = parseDecimal(valueText);
  const minimum = parseDecimal(signal.minimum);
  const maximum = parseDecimal(signal.maximum);
  assert(compareRational(value, minimum) >= 0 && compareRational(value, maximum) <= 0,
      `${signal.key}: vector value ${valueText} is outside DBC range`);
  const raw = nearestIntegerForNonTie(
      divideRational(subtractRational(value, parseDecimal(signal.offset)), parseDecimal(signal.factor)));
  const maximumRaw = bitMask(signal.length);
  assert(raw >= 0n && raw <= maximumRaw,
      `${signal.key}: encoded raw value ${raw} is outside ${signal.length}-bit range`);
  return raw;
}

function packVector(layout, values) {
  const expectedKeys = layout.signals.map((signal) => signal.key).sort();
  const actualKeys = Object.keys(values || {}).sort();
  assert.deepEqual(actualKeys, expectedKeys, 'vector fields must exactly match the declared layout');
  let payload = 0n;
  const raws = new Map();
  for (const signal of layout.signals) {
    const raw = encodeSignal(signal, values[signal.key]);
    raws.set(signal.key, raw);
    payload |= raw << BigInt(signal.start_bit);
  }
  for (const range of layout.zeroed_unassigned_bit_ranges || []) {
    const [first, last] = range;
    const rangeMask = bitMask(last - first + 1) << BigInt(first);
    assert.equal(payload & rangeMask, 0n, 'reference packer did not zero an unassigned bit range');
  }
  for (const signal of layout.signals) {
    const decodedRaw = (payload >> BigInt(signal.start_bit)) & bitMask(signal.length);
    assert.equal(decodedRaw, raws.get(signal.key), `${signal.key}: reference unpack disagrees with pack`);
  }
  return payloadToHex(payload);
}

function expectScaleDriftToFail(manifest, dbcText, records) {
  const needle = 'SG_ ADU_Tx_MCU01MotTqReq : 8|14@1+ (0.1,-800)';
  const altered = dbcText.replace(needle, 'SG_ ADU_Tx_MCU01MotTqReq : 8|14@1+ (0.2,-800)');
  assert.notEqual(altered, dbcText, 'negative scale-drift fixture could not be constructed');
  const mcu01 = records.find((record) => record.name === 'ADU_Tx_MCU01');
  assert(mcu01, 'manifest lacks ADU_Tx_MCU01 for negative scale-drift check');
  assert.throws(
      () => assertMessageMatchesDbc(mcu01, manifest, parseDbc(altered)),
      /factor drifted/,
      'an in-memory DBC scale drift must fail the layout contract');
}

function main() {
  const manifest = readJson(manifestPath);
  const vectors = readJson(vectorsPath);
  assert.equal(manifest.format, 'mine-teleop-jyr010-dbc-codec-manifest-v1');
  assert.equal(vectors.format, 'mine-teleop-jyr010-dbc-codec-vectors-v1');
  assert.equal(path.resolve(path.dirname(vectorsPath), vectors.manifest), manifestPath,
      'vector manifest path does not resolve to the checked manifest');

  for (const source of manifest.sources) {
    const sourcePath = path.join(repoRoot, source.path);
    assert.equal(sha256(sourcePath), source.sha256, `${source.path}: source hash drifted`);
  }

  for (const [layoutName, layout] of Object.entries(manifest.layouts)) {
    assertLayout(layoutName, layout);
  }
  const torqueSignal = manifest.layouts.mcu_command.signals.find(
      (signal) => signal.key === 'motor_torque_nm');
  assert(torqueSignal, 'MCU torque signal is missing from the manifest');
  assert.throws(
      () => encodeSignal(torqueSignal, '0.05'),
      /half-step physical reference values/,
      'the reference checker must not silently invent a half-step rounding rule');

  const dbcPath = path.join(repoRoot, manifest.sources[0].path);
  const dbcText = fs.readFileSync(dbcPath, 'utf8');
  const dbcMessages = parseDbc(dbcText);
  const records = resolveMessages(manifest);
  assert.equal(records.length, 16, 'R14 transmit manifest must cover all 16 emitted frames');
  for (const record of records) assertMessageMatchesDbc(record, manifest, dbcMessages);
  expectScaleDriftToFail(manifest, dbcText, records);

  const recordsByName = new Map(records.map((record) => [record.name, record]));
  const vectorsByName = new Map();
  for (const vector of vectors.vectors || []) {
    assert.equal(typeof vector.name, 'string', 'vector name must be a string');
    assert(!vectorsByName.has(vector.name), `duplicate vector name: ${vector.name}`);
    const record = recordsByName.get(vector.message);
    assert(record, `${vector.name}: vector names a message outside the R14 transmit manifest`);
    assert.equal(typeof vector.expected_data_hex, 'string');
    assert.match(vector.expected_data_hex, /^[0-9a-f]{16}$/,
        `${vector.name}: golden payload must be exactly 8 lowercase hex bytes`);
    const actual = packVector(manifest.layouts[record.layout], vector.values);
    assert.equal(actual, vector.expected_data_hex,
        `${vector.name}: independent reference payload differs from golden payload`);
    vectorsByName.set(vector.name, vector);
  }
  assert(vectorsByName.size > records.length, 'vectors must include at least one non-runtime reference boundary');

  for (const runtimeCase of vectors.runtime_cases || []) {
    assert.equal(runtimeCase.expected_state, 'ready');
    const expectedNames = runtimeCase.expected_vector_names || [];
    assert.equal(expectedNames.length, records.length,
        `${runtimeCase.name}: runtime case must compare all 16 emitted frames`);
    assert.equal(new Set(expectedNames).size, expectedNames.length,
        `${runtimeCase.name}: runtime vector names must be unique`);
    for (const name of expectedNames) {
      const vector = vectorsByName.get(name);
      assert(vector, `${runtimeCase.name}: unknown runtime vector ${name}`);
      assert(recordsByName.has(vector.message), `${runtimeCase.name}: vector has no manifest message`);
    }
  }

  console.log(
      `jyr010_dbc_contract=passed messages=${records.length} vectors=${vectorsByName.size} ` +
      `dbc_sha256=${manifest.sources[0].sha256} xls_sha256=${manifest.sources[1].sha256} ` +
      'scale_drift=detected half_step=unverified');
}

main();
