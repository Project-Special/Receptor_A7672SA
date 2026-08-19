// A7672SA Controller — Service Worker
const CACHE = 'a7672sa-v19';
const PRECACHE = [
  './index.html',
  './manifest.json',
  './icon.svg',
  'https://unpkg.com/leaflet@1.9.4/dist/leaflet.css',
  'https://unpkg.com/leaflet@1.9.4/dist/leaflet.js',
];

self.addEventListener('install', e => {
  e.waitUntil(
    caches.open(CACHE)
      .then(c => Promise.allSettled(PRECACHE.map(u => c.add(u))))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys.filter(k => k !== CACHE).map(k => caches.delete(k))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', e => {
  const url = new URL(e.request.url);

  // WebSocket do bridge — nunca interceptar
  if (e.request.url.startsWith('ws://') || e.request.url.startsWith('wss://')) return;

  // Tiles do mapa — network-first, sem poluir o cache
  if (url.hostname.endsWith('tile.openstreetmap.org')) {
    e.respondWith(fetch(e.request).catch(() => caches.match(e.request)));
    return;
  }

  // Documento HTML — network-first, senão o app trava numa versão antiga
  // do index.html mesmo depois de editar o arquivo.
  const isDoc = e.request.mode === 'navigate'
    || e.request.destination === 'document'
    || url.pathname.endsWith('.html');

  if (isDoc) {
    e.respondWith(
      fetch(e.request)
        .then(resp => {
          if (resp && resp.status === 200) {
            const clone = resp.clone();
            caches.open(CACHE).then(c => c.put(e.request, clone));
          }
          return resp;
        })
        .catch(() => caches.match(e.request).then(c => c || caches.match('./index.html')))
    );
    return;
  }

  // Demais recursos — cache-first com atualização em background
  e.respondWith(
    caches.match(e.request).then(cached => {
      const network = fetch(e.request).then(resp => {
        if (resp && resp.status === 200) {
          const clone = resp.clone();
          caches.open(CACHE).then(c => c.put(e.request, clone));
        }
        return resp;
      }).catch(() => cached || new Response('Offline', { status: 503 }));
      return cached || network;
    })
  );
});
