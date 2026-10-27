'use strict';
// Explicit bench entry; never included in desktop release bundles.
const {app, BrowserWindow, ipcMain, session, screen} = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const {execFileSync} = require('node:child_process');
const args = Object.fromEntries(process.argv.slice(2).reduce((out, value, i, all) => {
  if (value.startsWith('--')) out.push([value.slice(2), all[i + 1]]);
  return out;
}, []));
const seconds = Number(args.seconds || 600);
if (process.platform !== 'darwin' || !['browser', 'native'].includes(args.mode) ||
    !/^http:\/\/127\.0\.0\.1:\d+\/$/.test(args.url || '') || !args.output || !args.config || !args.probe ||
    !Number.isInteger(seconds) || seconds < 15 || seconds > 1800) throw Error('requires macOS, --mode browser|native --url http://127.0.0.1:PORT/ --config SAFE_BENCH --probe BINARY --output NEW_DIR [--seconds 15..1800] [--addon FILE]');
execFileSync(args.probe, ['--config', args.config, '--validate-only'], {stdio: 'pipe'});
fs.mkdirSync(args.output); // Refuse overwriting evidence.
app.setPath('userData', path.join(args.output, 'electron-profile'));
app.setName('MineTeleop Media Bench');
let window, addon, poll, timeout, closing = false, browserReport, nativeReport, layouts = 0, lastViews = [], resources;
const resourceSamples = [];
const events = [];
function log(value) {
  const row = {wall_ms: Date.now(), ...value}; events.push(row);
  fs.appendFileSync(path.join(args.output, 'events.jsonl'), JSON.stringify(row) + '\n');
}
function write(name, value) { fs.writeFileSync(path.join(args.output, name), JSON.stringify(value, null, 2)); }
async function finish(error) {
  if (closing) return; closing = true;
  clearTimeout(timeout); clearInterval(resources);
  if (error) log({event: 'error', error: String(error)});
  try {
    if (window && !window.isDestroyed()) {
      if (error) await window.webContents.executeJavaScript(`window.stopHybridBench?.(${JSON.stringify(String(error))})`).catch(() => {});
      await window.webContents.capturePage().then(image => fs.writeFileSync(path.join(args.output, 'web-content.png'), image.toPNG()))
        .catch(error => log({event: 'error', error: `web-content capture failed: ${error}`}));
      // No control commands; terminate the media session via the existing API.
      await window.webContents.executeJavaScript("post('/api/end-session',safeLogoutReleaseBody(pageReleaseScope(),'media_bench_finished')).catch(()=>{})").catch(() => {});
    }
    if (addon) {
      addon.stop();
      const deadline = Date.now() + 10000;
      while (!nativeReport && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 25));
      if (!nativeReport) log({event: 'error', error: 'native shutdown timed out'});
    }
    write('resources.json', resourceSamples);
    write('metadata.json', {mode: args.mode, duration_requested_s: seconds, versions: process.versions,
      screen: window && !window.isDestroyed() ? window.getBounds() : null,
      errors: events.filter(e => e.event === 'error'), browser_report: Boolean(browserReport), native_report: Boolean(nativeReport),
      screenshot_note: 'web-content.png excludes embedded NSViews; use a window screenshot to verify native pixels'});
  } finally {
    clearInterval(poll);
    if (window && !window.isDestroyed()) window.destroy();
    app.exit(error || !browserReport || events.some(e => e.event === 'error') || (args.mode === 'native' && !nativeReport?.passed) ? 1 : 0);
  }
}
process.on('SIGTERM', () => void finish('bench runner requested stop'));
process.on('SIGINT', () => void finish('bench interrupted'));
app.on('window-all-closed', () => { void finish('window closed before completion'); });
app.on('before-quit', event => { if (!closing) { event.preventDefault(); void finish('application quit before completion'); } });
app.whenReady().then(async () => {
  const origin = new URL(args.url).origin;
  session.defaultSession.setPermissionRequestHandler((_contents, _permission, callback) => callback(false));
  window = new BrowserWindow({title: 'MineTeleop 媒体台架（控制关闭）', width: 1500, height: 960, show: false,
    webPreferences: {preload: path.join(__dirname, 'preload.cjs'), nodeIntegration: false, contextIsolation: true,
      sandbox: true, webSecurity: true, backgroundThrottling: true, spellcheck: false}});
  // The Orin LAN may be a non-default interface (e.g. with a VPN active).
  // The default-route-only policy can silently force ICE-TCP in this bench.
  window.webContents.setWebRTCIPHandlingPolicy('default');
  window.webContents.setWindowOpenHandler(() => ({action: 'deny'}));
  window.webContents.on('will-navigate', (event, target) => { if (target !== args.url) event.preventDefault(); });
  window.webContents.on('will-attach-webview', event => event.preventDefault());
  window.webContents.on('render-process-gone', (_event, details) => void finish(`renderer gone: ${details.reason}`));
  window.webContents.on('console-message', details => {
    if (details.level === 'error') log({event: 'console_error', message: details.message});
  });
  window.on('close', event => { if (!closing) { event.preventDefault(); void finish('window closed before completion'); } });
  ipcMain.handle('media-bench', async (event, message) => {
    if (event.sender !== window.webContents || event.senderFrame !== window.webContents.mainFrame ||
        new URL(event.senderFrame.url).origin !== origin) throw Error('invalid bench sender');
    if (!message || typeof message !== 'object' || JSON.stringify(message).length > 128 * 1024 * 1024) throw Error('invalid bench message');
    if (message.type === 'offer' || message.type === 'candidate') {
      if (!addon || closing) throw Error('native receiver unavailable');
      addon.command(JSON.stringify({op: message.type, payload: message.payload}));
    } else if (message.type === 'layout') {
      if (addon && !closing) {
        const views = lastViews = JSON.parse(addon.layout(JSON.stringify(message.rects)));
        if (layouts++ < 15) log({event: 'native_layout', rects: message.rects, views});
      }
    } else if (message.type === 'ready') {
      if (addon && (lastViews.length !== 2 || lastViews.some(v => v.hidden || v.children.length !== 1))) throw Error('both native views must be visibly attached before measurement');
      clearTimeout(timeout);
      log({event: 'measurement_started', mode: args.mode, browser_clock_ms: message.at, selected_pairs: message.selectedPairs});
      timeout = setTimeout(() => void finish('measurement watchdog expired'), (seconds + 30) * 1000);
      return addon ? JSON.parse(addon.clock()) : null;
    } else if (message.type === 'complete') {
      browserReport = message.data; write('ui.json', browserReport);
      void finish(message.data.errors?.length ? message.data.errors.join('; ') : undefined);
    } else if (message.type === 'error') { void finish(String(message.error)); }
    else throw Error('unsupported bench operation');
    return true;
  });
  await window.loadURL(args.url);
  if (args.mode === 'native') {
    addon = require(path.resolve(args.addon));
    log({event: 'native_created', ...JSON.parse(addon.create(window.getNativeWindowHandle(), args.config))});
    poll = setInterval(() => {
      try {
        for (const event of JSON.parse(addon.poll())) {
          if (event.type === 'complete') { nativeReport = event.report; write('native.json', nativeReport); if (!closing) void finish(nativeReport.error || 'native receiver stopped early'); }
          else if (!window.isDestroyed()) window.webContents.send('media-bench-event', event);
        }
      } catch (error) { void finish(error); }
    }, 10);
  }
  resources = setInterval(() => { if (resourceSamples.length < 400) resourceSamples.push({wall_ms: Date.now(), processes: app.getAppMetrics()}); }, 5000);
  window.show(); window.focus();
  const script = fs.readFileSync(path.join(__dirname, 'renderer.js'), 'utf8');
  await window.webContents.executeJavaScript(`(${script})(${JSON.stringify({mode: args.mode, seconds})})`);
  log({event: 'page_ready', title: window.webContents.getTitle(), url: args.url, bounds: window.getBounds(),
    display: screen.getDisplayMatching(window.getBounds()), gpu_features: app.getGPUFeatureStatus(), window_id: window.getMediaSourceId(),
    webrtc_ip_handling_policy: window.webContents.getWebRTCIPHandlingPolicy()});
  timeout = setTimeout(() => void finish('login/stream startup timed out'), 90000);
  if (process.env.MINE_TELEOP_DRIVER_PASSWORD) {
    await window.webContents.executeJavaScript(`document.getElementById('driver-id').value='native-bench';document.getElementById('password').value=${JSON.stringify(process.env.MINE_TELEOP_DRIVER_PASSWORD)};document.getElementById('login').click();void 0`);
    const connect = setInterval(async () => {
      if (closing) return clearInterval(connect);
      const ready = await window.webContents.executeJavaScript("document.getElementById('vehicle').options.length > 0").catch(() => false);
      if (ready) { clearInterval(connect); await window.webContents.executeJavaScript("document.getElementById('connect').click();void 0"); }
    }, 500);
  }
}).catch(error => void finish(error));
