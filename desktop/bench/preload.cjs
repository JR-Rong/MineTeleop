'use strict';
const {contextBridge, ipcRenderer} = require('electron');
contextBridge.exposeInMainWorld('mineTeleopBench', {
  send: message => ipcRenderer.invoke('media-bench', message),
  onEvent: callback => ipcRenderer.on('media-bench-event', (_event, value) => callback(value)),
});
