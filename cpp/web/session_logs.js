(function (root) {
  'use strict';
  function sessionLabel(session) {
    return `${session.session_id} · ${session.vehicle_id} · ${new Date(session.started_at_utc_ms).toLocaleString()}`;
  }
  function exportStatusMessage(value) {
    if (value.state === 'collecting') {
      const progress = value.vehicle_progress || {};
      const detail = progress.claimed ? `车端已接任务，已接收 ${(Number(progress.bytes_received || 0) / 1048576).toFixed(1)} MB。` : '等待车端接收采集任务。';
      return `正在收集三端日志。${detail}请保持车辆空闲、三端在线；最长约 10 分钟。`;
    }
    const manifest=value.manifest||{},recording = value.recording || manifest.recording || (manifest.vehicle||{}).recording || {};
    const summary=recording.summary||{},gap=recording.evidence_status==='incomplete'||recording.incomplete===true||summary.incomplete===true;
    const confirmedGap = Array.isArray(summary.cameras) && summary.cameras.some(camera => camera && camera.recording &&
      ['recording_admission_overflow','recording_invalid_access_unit','recording_disk_full'].includes(camera.recording.issue_code));
    const recordingMessage = recording.included === false && gap
      ? confirmedGap ? '录像不完整；已确认的此前缺口无法补录。下一步：车辆空闲后检查存储空间、目录权限和录制日志。'
        : '录像证据不足，完整性待确认。下一步：保留已有片段和日志，检查车端封装证据、存储和录制状态。'
      : recording.included === false && recording.evidence_status === 'finalized' && recording.complete === true &&
          summary.completeness_scope==='full_session' && summary.session_history_known===true
        ? '车端会话摘要确认整会话录像完整。'
        : recording.included === false && recording.evidence_status === 'attempt_finalized' && recording.attempt_complete === true
          ? '车端摘要报告本次录像封装完成；整会话录像完整性未确认。'
        : recording.included === false && recording.evidence_status === 'not_enabled'
          ? '该会话未启用录像。'
          : '录像完整性未确认。下一步：检查车端会话录制摘要、存储和车端版本。';
    const scopeMessage = 'ZIP 仅包含日志和录制状态摘要，不包含视频文件。';
    if (value.logs_complete === true || value.logs_complete === undefined && value.complete === true)
      return `三端日志收集完成，可以保存 ZIP。${recordingMessage}${scopeMessage}`;
    const reasons = {
      vehicle_offline: '车端离线',
      vehicle_worker_unresponsive: '车端未响应采集任务，请检查车端版本及日志采集进程',
      vehicle_upload_timeout: '车端上传中断或超时',
      vehicle_collection_failed: '车端读取日志失败',
      vehicle_upload_failed: '车端上传失败，重试后仍未完成',
      vehicle_finish_failed: '车端上传完成确认失败',
      new_control_session_started: '车辆重新进入控制会话，日志采集已停止',
      vehicle_timeout_offline_or_unsupported_version: '车端未完成采集，请检查在线状态及版本',
      cloud_log_read_failed: '云端读取日志失败',
    };
    const labels = {vehicle: '车端', cloud: '云端', controller: '控制端'};
    const missing = Object.entries(labels).flatMap(([source, label]) => {
      const report = value.manifest && value.manifest[source];
      if (!report || report.status === 'available') return [];
      if (report.reason) return [reasons[report.reason] || `${label}日志不完整（${report.reason}）`];
      const files = (report.files || []).filter(file => !file.optional && file.status !== 'available');
      return files.length ? [`${label}日志缺失或截断：${files.map(file => file.name).join('、')}`] : [];
    });
    return `仅收集到部分日志：${missing.join('；') || '可选日志缺失或截断'}。可以保存现有日志，详细原因见 ZIP 清单。${recordingMessage}${scopeMessage}`;
  }
  function install() {
    const button = document.getElementById('export-session-logs');
    if (!button) return;
    const dialog = document.createElement('dialog');
    dialog.id = 'session-log-dialog';
    dialog.innerHTML = '<h2>导出会话日志</h2><p>选择已结束的会话，收集车端、控制端和云端日志。车端需保持在线；缺失或截断信息会写入 ZIP 清单。录像完整性单独报告，视频文件不包含在 ZIP 中。</p>' +
      '<label>会话 <select id="log-session-select" style="width:100%;margin:12px 0"></select></label>' +
      '<p id="log-export-message" role="status" aria-live="polite"></p>' +
      '<div><button id="log-export-start">收集三端日志</button> <button id="log-export-download" hidden>保存 ZIP</button> <button id="log-export-close">关闭</button></div>';
    document.body.appendChild(dialog);
    const select = dialog.querySelector('select');
    const message = dialog.querySelector('#log-export-message');
    const start = dialog.querySelector('#log-export-start');
    const download = dialog.querySelector('#log-export-download');
    let timer = null, filename = 'mine-teleop-session.zip';
    async function request(url, body) {
      const response = await fetch(url, body === undefined ? {cache: 'no-store'} : {
        method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(body)
      });
      const value = await response.json();
      if (!response.ok) throw Error(value.error || `请求失败 (${response.status})`);
      return value;
    }
    function fail(error) { message.textContent = error.message; start.disabled = !select.value; }
    async function poll() {
      clearTimeout(timer);
      try {
        const value = await request('/api/log-export');
        download.hidden = value.state !== 'ready';
        start.disabled = value.state === 'collecting' || !select.value;
        if (value.state === 'collecting') {
          message.textContent = exportStatusMessage(value);
          if (dialog.open) timer = setTimeout(poll, 1500);
        } else if (value.state === 'ready') {
          filename = value.filename;
          message.textContent = exportStatusMessage(value);
        } else if (value.state === 'failed') message.textContent = value.message;
      } catch (error) { fail(error); }
    }
    button.onclick = async () => {
      dialog.showModal(); message.textContent = '正在读取会话列表…'; start.disabled = true;
      select.replaceChildren(); download.hidden = true;
      try {
        const status = await request('/api/status');
        if (status.connected) throw Error('请先结束当前控制会话，再导出日志。');
        const result = await request('/api/log-sessions');
        for (const session of result.sessions) {
          const option = document.createElement('option'); option.value = session.key;
          option.textContent = sessionLabel(session); select.appendChild(option);
        }
        message.textContent = result.sessions.length ? '请选择会话。导出仅包含当前驾驶员有权限查看的日志。' : '暂无已结束会话。升级三端后产生的会话会出现在这里，目录保留 7 天。';
        start.disabled = !select.value; await poll();
      } catch (error) { fail(error); }
    };
    start.onclick = async () => {
      start.disabled = true; download.hidden = true;
      try { await request('/api/log-export', {key: select.value}); await poll(); }
      catch (error) { fail(error); }
    };
    download.onclick = async () => {
      download.disabled = true;
      try {
        const response = await fetch('/api/log-export/download', {cache: 'no-store'});
        if (!response.ok) throw Error('下载失败，请确认当前登录仍有效。');
        const url = URL.createObjectURL(await response.blob());
        const link = document.createElement('a'); link.href = url; link.download = filename;
        document.body.appendChild(link); link.click(); link.remove();
        setTimeout(() => URL.revokeObjectURL(url), 60000);
      } catch (error) { fail(error); }
      finally { download.disabled = false; }
    };
    dialog.querySelector('#log-export-close').onclick = () => dialog.close();
    dialog.addEventListener('close', () => clearTimeout(timer));
  }
  root.MineTeleopSessionLogs = {install, sessionLabel, exportStatusMessage};
  if (typeof module !== 'undefined') module.exports = {sessionLabel, exportStatusMessage};
})(typeof globalThis !== 'undefined' ? globalThis : this);
