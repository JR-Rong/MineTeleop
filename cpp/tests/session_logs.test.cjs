'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const {exportStatusMessage} = require('../web/session_logs.js');

test('shows whether the vehicle claimed a task and made upload progress', () => {
  assert.match(exportStatusMessage({state: 'collecting'}), /等待车端/);
  assert.match(exportStatusMessage({state: 'collecting', vehicle_progress: {claimed: true, bytes_received: 2097152}}), /2\.0 MB/);
});
test('an empty vehicle export is explicitly incomplete and gives the failure stage', () => {
  for (const [reason, text] of [['vehicle_worker_unresponsive', '未响应'], ['vehicle_upload_failed', '上传失败'], ['vehicle_upload_timeout', '超时']]) {
    const message = exportStatusMessage({state: 'ready', complete: false, manifest: {vehicle: {status: 'partial', reason, files: []}}});
    assert.match(message, /仅收集到部分日志/);
    assert.ok(message.includes(text));
  }
});
test('reports missing required files and treats optional logs separately', () => {
  const message = exportStatusMessage({state: 'ready', complete: false, manifest: {vehicle: {status: 'partial', files: [{name: 'runtime.log', status: 'partial'}]}}});
  assert.match(message, /runtime\.log/);
  assert.match(exportStatusMessage({state: 'ready', complete: true}), /收集完成/);
});
