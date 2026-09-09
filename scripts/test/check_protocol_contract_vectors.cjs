'use strict';

const assert = require('assert').strict;
const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '../..');
const manifestPath = path.join(
    repoRoot, 'protocol', 'v1', 'fixtures', 'control-command-vectors.json');
const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
const schemaPath = path.resolve(path.dirname(manifestPath), manifest.schema);
const schema = JSON.parse(fs.readFileSync(schemaPath, 'utf8'));

function isExtensionKeyword(key) {
  return key.startsWith('x-');
}

function hasOwn(value, key) {
  return Object.prototype.hasOwnProperty.call(value, key);
}

function validateSchemaShape(value) {
  const rootKeywords = new Set([
    '$schema', '$id', 'title', 'description', '$comment', 'type', 'required',
    'properties', 'additionalProperties',
  ]);
  const propertyKeywords = new Set([
    'title', 'description', '$comment', 'type', 'const', 'enum', 'minimum',
    'maximum', 'minLength', 'default',
  ]);
  assert.equal(typeof value, 'object');
  assert.notEqual(value, null);
  for (const key of Object.keys(value)) {
    assert(rootKeywords.has(key) || isExtensionKeyword(key),
        `unsupported root schema keyword: ${key}`);
  }
  assert.equal(value.type, 'object');
  assert(Array.isArray(value.required), 'schema required must be an array');
  assert.equal(typeof value.properties, 'object');
  assert.notEqual(value.properties, null);
  assert.equal(value.additionalProperties, true,
      'v1 schema must explicitly retain unknown-field compatibility');

  for (const required of value.required) {
    assert.equal(typeof required, 'string');
    assert(hasOwn(value.properties, required),
        `required field lacks a property definition: ${required}`);
  }
  for (const [name, property] of Object.entries(value.properties)) {
    assert.equal(typeof property, 'object', `property is not an object: ${name}`);
    assert.notEqual(property, null, `property is null: ${name}`);
    assert.equal(typeof property.type, 'string', `property has no type: ${name}`);
    for (const key of Object.keys(property)) {
      assert(propertyKeywords.has(key) || isExtensionKeyword(key),
          `unsupported property schema keyword: ${name}.${key}`);
    }
  }
}

function matchesType(value, expected) {
  switch (expected) {
    case 'object': return typeof value === 'object' && value !== null && !Array.isArray(value);
    case 'string': return typeof value === 'string';
    case 'boolean': return typeof value === 'boolean';
    case 'number': return typeof value === 'number' && Number.isFinite(value);
    case 'integer': return typeof value === 'number' && Number.isSafeInteger(value);
    default: throw new Error(`unsupported JSON Schema type in contract test: ${expected}`);
  }
}

function sameJson(left, right) {
  return JSON.stringify(left) === JSON.stringify(right);
}

function validateProperty(property, value, field) {
  if (!matchesType(value, property.type)) return `wrong type for ${field}`;
  if (hasOwn(property, 'const') && !sameJson(value, property.const)) {
    return `wrong const value for ${field}`;
  }
  if (hasOwn(property, 'enum') && !property.enum.some((candidate) => sameJson(value, candidate))) {
    return `value outside enum for ${field}`;
  }
  if (hasOwn(property, 'minLength') && [...value].length < property.minLength) {
    return `string shorter than minLength for ${field}`;
  }
  if (hasOwn(property, 'minimum') && value < property.minimum) {
    return `number below minimum for ${field}`;
  }
  if (hasOwn(property, 'maximum') && value > property.maximum) {
    return `number above maximum for ${field}`;
  }
  return null;
}

function validateMessage(message) {
  if (typeof message !== 'object' || message === null || Array.isArray(message)) {
    return 'message is not an object';
  }
  for (const field of schema.required) {
    if (!hasOwn(message, field)) return `missing required field: ${field}`;
  }
  for (const [field, value] of Object.entries(message)) {
    if (!hasOwn(schema.properties, field)) {
      if (!schema.additionalProperties) return `unknown field: ${field}`;
      continue;
    }
    const error = validateProperty(schema.properties[field], value, field);
    if (error) return error;
  }
  return null;
}

function readVectorValue(vector) {
  const hasFile = hasOwn(vector, 'file');
  const hasValue = hasOwn(vector, 'value');
  assert.notEqual(hasFile, hasValue, `${vector.name}: exactly one of file or value is required`);
  if (hasFile) {
    assert.equal(typeof vector.file, 'string', `${vector.name}: file must be a string`);
    return JSON.parse(fs.readFileSync(path.resolve(path.dirname(manifestPath), vector.file), 'utf8'));
  }
  return vector.value;
}

validateSchemaShape(schema);
assert.equal(manifest.format, 'mine-teleop-control-command-contract-v1');
assert.equal(manifest.scope, 'structural-only');
assert(Array.isArray(manifest.vectors));
assert.equal(manifest.numeric_compatibility.portable_json_integer_maximum, Number.MAX_SAFE_INTEGER);
assert.equal(schema['x-portable-json-integer-maximum'], Number.MAX_SAFE_INTEGER);
assert.equal(schema.properties.seq.maximum, Number.MAX_SAFE_INTEGER);
assert.equal(schema.properties.sent_at_utc_ms.maximum, Number.MAX_SAFE_INTEGER);

let valid = 0;
for (const vector of manifest.vectors) {
  assert.equal(typeof vector.name, 'string');
  assert.equal(typeof vector.structural_valid, 'boolean');
  const result = validateMessage(readVectorValue(vector));
  assert.equal(!result, vector.structural_valid,
      `${vector.name}: expected structural_valid=${vector.structural_valid}, got ${result || 'valid'}`);
  if (vector.structural_valid) valid += 1;
}

assert(valid > 0, 'protocol contract has no valid vectors');
console.log(`protocol_v1_schema_js_contract=passed vectors=${manifest.vectors.length} valid=${valid}`);
