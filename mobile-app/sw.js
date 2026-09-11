'use strict';
const CACHE = 'mine-teleop-mobile-v2';
const SHELL = ['/mobile/','/mobile/app.css','/mobile/app.js','/mobile/icon.svg','/mobile/icon-192.png','/mobile/icon-512.png','/mobile/manifest.webmanifest'];
self.addEventListener('install',event => event.waitUntil(caches.open(CACHE).then(cache => cache.addAll(SHELL))));
self.addEventListener('activate',event => event.waitUntil(caches.keys().then(keys => Promise.all(keys.filter(key => key.startsWith('mine-teleop-mobile-') && key !== CACHE).map(key => caches.delete(key)))).then(() => self.clients.claim())));
self.addEventListener('fetch',event => {
  const url = new URL(event.request.url);
  if (url.origin !== self.location.origin || event.request.method !== 'GET' || !SHELL.includes(url.pathname)) return;
  event.respondWith(fetch(event.request).catch(() => caches.match(event.request).then(response => response || Response.error())));
});
