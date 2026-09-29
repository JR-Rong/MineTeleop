(function (root) {
  'use strict';
  function sessionLabel(session) {
    return `${session.session_id} · ${session.vehicle_id} · ${new Date(session.started_at_utc_ms).toLocaleString()}`;
  }
  function install() {
    const button = document.getElementById('export-session-logs');
    if (!button) return;
    const dialog = document.createElement('dialog');
    dialog.id = 'session-log-dialog';
    dialog.innerHTML = '<h2>导出会话日志</h2><p>选择已结束的会话，收集车端、控制端和云端日志。车端需保持在线；缺失或截断信息会写入 ZIP 清单。</p>' +
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
          message.textContent = '正在收集三端日志，请保持车端和控制端在线。最多等待约 3 分钟。';
          if (dialog.open) timer = setTimeout(poll, 1500);
        } else if (value.state === 'ready') {
          filename = value.filename;
          message.textContent = value.complete ? '收集完成，可以保存 ZIP。' : '收集完成，但存在缺失或截断。ZIP 内 manifest.json 列出了各端原因。';
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
  root.MineTeleopSessionLogs = {install, sessionLabel};
  if (typeof module !== 'undefined') module.exports = {sessionLabel};
})(typeof globalThis !== 'undefined' ? globalThis : this);
