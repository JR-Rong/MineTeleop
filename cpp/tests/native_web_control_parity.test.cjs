'use strict';

// Compare the native implementation with the actual shipped web reducer.
const assert = require('assert').strict;
const {spawnSync} = require('child_process');
const {deriveControl} = require('../web/control_logic.js');

const inputs = [];
const profiles = [
  {maxThrottle: 0, maxBrakePressureBar: 163.8, serviceBrakePressureBar: 30, hardBrakePressureBar: 80, maxSteeringDeg: 0},
  {maxThrottle: .45, maxBrakePressureBar: 100, serviceBrakePressureBar: 30, hardBrakePressureBar: 80, maxSteeringDeg: 15},
  {maxThrottle: 2, maxBrakePressureBar: 0, serviceBrakePressureBar: 20, hardBrakePressureBar: 40, maxSteeringDeg: -1},
];
for (const limits of profiles) for (const selectedGear of ['N', 'D', 'R']) {
  for (let mask = 0; mask < 64; ++mask) for (const gamepad of [
    {steering: 0, throttle: 0, brake: 0}, {steering: -.4, throttle: .7, brake: .2},
  ]) {
    const keyState = Object.fromEntries(['up', 'down', 'left', 'right', 'service_brake', 'hard_brake']
      .map((key, i) => [key, Boolean(mask & (1 << i))]));
    inputs.push({keyState, gamepad, selectedGear, limits, steeringFullScaleDeg: 30, estop: false});
  }
}
const lines = inputs.map(({keyState: k, gamepad: p, selectedGear: g, limits: l, steeringFullScaleDeg: s, estop: e}) =>
  [k.up, k.down, k.left, k.right, k.service_brake, k.hard_brake].map(Number).concat(
    [p.steering, p.throttle, p.brake, g, l.maxThrottle, l.maxBrakePressureBar,
      l.serviceBrakePressureBar, l.hardBrakePressureBar, l.maxSteeringDeg, s, Number(e)]).join(' ')).join('\n');
const run = spawnSync(process.argv[2], {input: lines, encoding: 'utf8', maxBuffer: 4 * 1024 * 1024});
assert.equal(run.status, 0, run.stderr);
const results = run.stdout.trim().split('\n');
assert.equal(results.length, inputs.length);
for (let i = 0; i < inputs.length; ++i) {
  const [gear, steering, throttle, brake, estop] = results[i].split(' ');
  const expected = deriveControl(inputs[i]);
  assert.equal(gear, expected.gear, `gear case ${i}`);
  for (const [name, actual] of Object.entries({steering, throttle, brake}))
    assert(Math.abs(Number(actual) - expected[name]) < 1e-12, `${name} case ${i}`);
  assert.equal(Boolean(Number(estop)), expected.estop, `estop case ${i}`);
}
console.log(`matched ${inputs.length} cases against the shipped web reducer`);
