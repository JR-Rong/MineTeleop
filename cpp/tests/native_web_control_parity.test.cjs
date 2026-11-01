'use strict';

// Compare the native implementation with the actual shipped web reducer.
const assert = require('assert').strict;
const {spawnSync} = require('child_process');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const {deriveControl, createGearTransition, recordForwardedGearCommand,
  telemetryConfirmsGearTransition, matchesGearChangeRejection} = require('../web/control_logic.js');

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

// Execute the actual browser sampler for standard-mapped gamepads. The fixture
// supplies authority and a selected gear so this compares only calibration.
const source = fs.readFileSync(path.join(__dirname, '../src/server.cpp'), 'utf8');
const names = ['clamp', 'applyDeadzone', 'applyPedalDeadzone', 'axisValue', 'buttonValue', 'sampleGamepad'];
const functions = names.map(name => {
  const match = source.match(new RegExp(`^function ${name}\\(.*$`, 'm'));
  assert(match, `missing shipped ${name} function`);
  return match[0];
}).join('\n');
const padCases = [];
for (const axis of [-1, -.5, -.05, 0, .1, .8, 1]) for (const throttle of [0, .04, .3, 1]) {
  for (const inverted of [false, true]) padCases.push({axis, throttle, brake: .35, center: .1, range: .8, deadzone: .05, inverted});
}
const padInput = padCases.map(p => [p.axis, p.throttle, p.brake, p.center, p.range, p.deadzone, Number(p.inverted)].join(' ')).join('\n');
const padRun = spawnSync(process.argv[2], ['--standard-gamepad'], {input: padInput, encoding: 'utf8'});
assert.equal(padRun.status, 0, padRun.stderr);
const padResults = padRun.stdout.trim().split('\n');
assert.equal(padResults.length, padCases.length);
padCases.forEach((p, i) => {
  const buttons = Array.from({length: 8}, () => ({value: 0}));
  buttons[6].value = p.brake; buttons[7].value = p.throttle;
  const context = vm.createContext({gamepadConfig: {enabled: true, axis_deadzone: p.deadzone,
      steering_inverted: p.inverted, estop_button: 0},
    document: {hidden: false, hasFocus: () => true}, firstConnectedGamepad: () => ({mapping: 'standard', axes: [p.axis], buttons}),
    calibration: {steeringCenter: p.center, steeringRange: p.range}, gamepadState: {},
    renderControlState: () => {}, vcuEverReady: true, applyGamepadNeutralInterlock: () => {},
    selectedGear: 'D', gamepadEstopPressedAt: 0});
  vm.runInContext(`${functions}\nsampleGamepad();`, context);
  const actual = padResults[i].split(' ').map(Number);
  for (const [index, name] of ['steering', 'throttle', 'brake'].entries())
    assert(Math.abs(actual[index] - context.gamepadState[name]) < 1e-12, `${name} gamepad case ${i}`);
});
console.log(`matched ${padCases.length} cases against the shipped standard gamepad sampler`);

const gearCases = [];
for (const status of [9, 10, 11]) for (const forwarded of [false, true])
  for (const actual of ['N', 'D', 'R']) for (const fresh of [false, true])
    for (const rejection of [0, 20, 21]) gearCases.push({status, forwarded, actual, fresh, rejection});
const gearInput = gearCases.map(x => [x.status, Number(x.forwarded), x.actual, Number(x.fresh), x.rejection].join(' ')).join('\n');
const gearRun = spawnSync(process.argv[2], ['--gear-transition'], {input: gearInput, encoding: 'utf8'});
assert.equal(gearRun.status, 0, gearRun.stderr);
const gearResults = gearRun.stdout.trim().split('\n');
assert.equal(gearResults.length, gearCases.length);
gearCases.forEach((x, i) => {
  let transition = createGearTransition('N', 'D', 10, 1);
  if (x.forwarded) transition = recordForwardedGearCommand(transition, 1, 20, 'D');
  const message = {event: 'vehicle_telemetry', control_status_seq: x.status, gear: x.actual,
    can_feedback: x.fresh ? {supported: false} : {supported: true, feedback_fresh: false}};
  const rejection = {issue_code: 'vcu_drive_gear_change_moving_or_stale',
    control_status_seq: x.status, intent_seq: x.rejection};
  const [confirmed, rejected] = gearResults[i].split(' ').map(Number);
  assert.equal(Boolean(confirmed), telemetryConfirmsGearTransition(transition, message), `gear confirmation case ${i}`);
  assert.equal(Boolean(rejected), matchesGearChangeRejection(transition, rejection, 'D'), `gear rejection case ${i}`);
});
console.log(`matched ${gearCases.length} cases against the shipped web gear transition logic`);
