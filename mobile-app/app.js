'use strict';
const $ = (id) => document.getElementById(id);
const labels = {pending:'等待审批',approved:'已同意',rejected:'已拒绝',expired:'已过期',cancelled:'已取消',consumed:'已用于连接'};
let token = '', epoch = 0, refreshSequence = 0, timer, busy = false, fresh = false, connectionFailed = false, records = [], snapshot = '', receivedAt = 0;
function message(text = '') { $('message').textContent = text; $('message').hidden = !text; }
async function api(path, body) {
  const response = await fetch('/mobile/api/' + path, {
    method: body === undefined ? 'GET' : 'POST', cache:'no-store', credentials:'omit',
    headers:{'Content-Type':'application/json', ...(token ? {'X-Mine-Teleop-Approver-Token':token} : {})},
    body:body === undefined ? undefined : JSON.stringify(body), signal:AbortSignal.timeout(8000)
  });
  const data = await response.json();
  if (!response.ok) { const error = new Error(data.error || '请求失败，请重试'); error.status = response.status; throw error; }
  return data;
}
function lockActions() {
  document.querySelectorAll('.actions button').forEach(button => {
    const record = records.find(item => item.request_id === button.dataset.request);
    button.disabled = busy || !fresh || !record || remaining(record) <= 0;
  });
}
function remaining(record) { return Math.max(0, record.remaining_ms - (performance.now() - receivedAt)); }
function clockLabel(ms) { const sec = Math.ceil(ms / 1000); return `${String(Math.floor(sec / 60)).padStart(2,'0')}:${String(sec % 60).padStart(2,'0')}`; }
function element(tag, text, className) { const node = document.createElement(tag); if (text) node.textContent = text; if (className) node.className = className; return node; }
function render() {
  // Preserve focus while polling; only rebuild when server state, not the countdown, changes.
  const key = JSON.stringify(records.map(({remaining_ms, ...rest}) => rest));
  if (snapshot === key) { lockActions(); return; }
  snapshot = key;
  const container = $('requests'); container.replaceChildren();
  if (!records.length) container.append(element('p','暂无控制申请','empty'));
  for (const record of records) {
    const article = element('article');
    article.append(element('p',labels[record.state] || '未知状态','state ' + (labels[record.state] ? record.state : '')));
    article.append(element('h3',record.vehicle_id));
    const details = element('dl');
    for (const [label,value] of [['驾驶员',record.driver_id],['剩余时间',clockLabel(remaining(record))]]) {
      const row = element('div'), dd = element('dd',value);
      if (label === '剩余时间') dd.dataset.countdown = record.request_id;
      row.append(element('dt',label),dd); details.append(row);
    }
    article.append(details);
    if (record.state === 'pending') {
      article.append(element('p','同意仅适用于本次连接申请。','note'));
      const actions = element('div',null,'actions');
      for (const [decision,label,style] of [['reject','拒绝','reject'],['approve','同意','primary']]) {
        const button = element('button',label,style); button.dataset.request = record.request_id;
        button.setAttribute('aria-label',`${label} ${record.vehicle_id} 的控制申请`);
        button.addEventListener('click',() => decide(record.request_id,decision)); actions.append(button);
      }
      article.append(actions);
    } else if (record.state === 'approved') article.append(element('p','已同意，控制端将自动连接。','result'));
    container.append(article);
  }
  lockActions();
}
function reset() {
  epoch++; refreshSequence++; clearTimeout(timer); token = ''; fresh = false; connectionFailed = false; busy = false; records = []; snapshot = '';
  $('requests').replaceChildren(); $('inbox').hidden = true; $('logout').hidden = true; $('login-panel').hidden = false;
  $('connection').textContent = '请保持页面打开以接收申请。'; $('password').value = ''; $('login').disabled = false;
}
async function refresh() {
  const current = epoch, sequence = ++refreshSequence; clearTimeout(timer);
  try {
    const data = await api('requests'); if (current !== epoch || sequence !== refreshSequence) return;
    records = data.requests; receivedAt = performance.now(); fresh = true; render();
    if (connectionFailed) message(); connectionFailed = false;
    $('connection').textContent = '已连接 · 自动刷新';
  } catch (error) {
    if (current !== epoch || sequence !== refreshSequence) return;
    fresh = false; connectionFailed = true; lockActions();
    if (error.status === 401) { reset(); message(error.message); return; }
    $('connection').textContent = '连接中断 · 正在重试'; message('暂时无法同步申请，恢复连接后才能审批。');
  } finally { if (current === epoch && sequence === refreshSequence && token) timer = setTimeout(refresh,3000); }
}
async function decide(id, decision) {
  if (busy || !fresh) return;
  const current = epoch; busy = true; lockActions(); message();
  try {
    await api(`requests/${encodeURIComponent(id)}/decision`,{decision});
    if (current !== epoch) return;
    await refresh();
  } catch (error) {
    if (current !== epoch) return;
    message(error.status ? error.message : '结果尚未确认，请刷新后核对，勿重复操作。');
    await refresh();
  } finally { if (current === epoch) { busy = false; lockActions(); } }
}
$('login-form').addEventListener('submit',async event => {
  event.preventDefault(); if ($('login').disabled) return;
  const current = ++epoch; $('login').disabled = true; message();
  const password = $('password').value; $('password').value = '';
  try {
    const data = await api('login',{password});
    if (current !== epoch) return;
    token = data.token; $('login-panel').hidden = true; $('inbox').hidden = false; $('logout').hidden = false;
    await refresh();
  } catch (error) { if (current === epoch) message(error.message); }
  finally { if (current === epoch) $('login').disabled = false; }
});
$('logout').addEventListener('click',async () => {
  $('logout').disabled = true;
  try { await api('logout',{}); reset(); message(); }
  catch (error) { if (error.status === 401) { reset(); message(); } else message('退出未完成，请恢复网络后重试。'); }
  finally { $('logout').disabled = false; }
});
$('show-password').addEventListener('click',() => {
  const show = $('password').type === 'password'; $('password').type = show ? 'text' : 'password';
  $('show-password').setAttribute('aria-pressed',String(show)); $('show-password').setAttribute('aria-label',show ? '隐藏密码' : '显示密码');
});
setInterval(() => {
  for (const node of document.querySelectorAll('[data-countdown]')) {
    const record = records.find(item => item.request_id === node.dataset.countdown);
    if (record) node.textContent = clockLabel(remaining(record));
  }
  lockActions();
},1000);
document.addEventListener('visibilitychange',() => { if (token && !document.hidden && !busy) refresh(); });
window.addEventListener('offline',() => { fresh = false; lockActions(); $('connection').textContent = '网络已断开'; });
// Cache only the public app shell. Authenticated API responses always require the network.
if ('serviceWorker' in navigator) navigator.serviceWorker.register('/mobile/sw.js',{scope:'/mobile/'}).catch(() => {});
