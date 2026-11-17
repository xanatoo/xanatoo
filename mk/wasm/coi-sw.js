// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Cross-origin isolation for hosts that cannot send headers (GitHub Pages).
// The threads build needs SharedArrayBuffer, which browsers only give a page
// served with COOP / COEP. This worker re-serves the site's own files with
// those headers; shell.html registers it and reloads once under its control.
// Cross-origin requests (the CORS fetches of the network readers) pass
// through untouched: COEP require-corp accepts CORS responses.

self.addEventListener('install', function () { self.skipWaiting(); });
self.addEventListener('activate', function (event) {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', function (event) {
  var request = event.request;
  if (new URL(request.url).origin !== self.location.origin) return;
  // A DevTools quirk: only-if-cached is only valid for same-origin mode.
  if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') return;
  event.respondWith(fetch(request).then(function (response) {
    if (response.status === 0) return response;   // opaque: cannot be copied
    var headers = new Headers(response.headers);
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    return new Response(response.body, {
      status: response.status, statusText: response.statusText, headers: headers
    });
  }));
});
