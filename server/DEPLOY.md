# Deploying tiles.waycast.io

The tile server is one stdlib-Python file; deployment is Docker +
Caddy (automatic Let's Encrypt TLS). Total cost: a ~$5/mo VPS + the
domain.

## 1. Domain (once)

Register **waycast.io** (Cloudflare Registrar sells at cost; Namecheap
fine too — grab waycast.org as well, it's cheap insurance). Then add
a DNS **A record**: `tiles.waycast.io -> <VPS IP>`.

## 2. VPS (once)

Any small box works (Hetzner CX22 ~€4/mo, DigitalOcean, etc.):

```sh
apt install -y docker.io docker-compose-v2 git
git clone <repo> && cd vehicular-mesh/server
docker compose up -d --build
```

Caddy obtains and renews TLS automatically once DNS resolves.
Health check: `curl https://tiles.waycast.io/healthz`

Optionally pre-seed the cache (saves the first users the misses):
`docker cp ../assets/tiles/. server-tiles-1:/data/cache/`

## 3. Point devices at it

`targets/esp32p4/main/tile_server.h`:

```c
#define TILE_SERVER_BASE "https://tiles.waycast.io"
```

(HTTPS works out of the box — the firmware carries the certificate
bundle, which includes Let's Encrypt's root.) Devices fall back to
OSM per-tile if the server is unreachable.

## Endpoints

- `GET /{z}/{x}/{y}.png` — single tile (cache-on-miss)
- `GET /area.tar?lat=..&lon=..[&half_lat=..][&half_lon=..]` — region
  archive, z13-16, server does the tile math (cap 1500 tiles)
- `GET /healthz` — liveness + cache size

## Scaling / product path

- The OSM upstream is a bootstrap convenience with per-IP budgets and
  global throttle — fine for a small beta fleet, not for thousands of
  devices. The product replacement is self-rendered tiles
  (planetiler / openmaptiles raster output) dropped into the cache
  dir — same paths, upstream removed.
- The `/area.tar` endpoint is the future device fast path (one HTTP
  round trip per region instead of hundreds); firmware-side untar is
  a small follow-up.
- Attribution: (c) OpenStreetMap contributors — required on the map
  UI (present) and echoed in the X-Attribution header.

---

## DEPLOYED (July 17 2026) — kda-hetzner-1

- Host: kda-hetzner-1 (46.4.48.244), via bastion:
  `ssh Alviso99@80.240.18.30 -t -- kda-hetzner-1`
- Layout follows the house pattern: host nginx owns 80/443,
  per-app docker containers on 81xx ports. Waycast tiles = **8140**.
- Files: `/root/waycast-tiles/` (tileserver.py, Dockerfile, deploy.sh,
  cache/ bind-mounted into the container). Redeploy = re-upload
  files + `bash /root/waycast-tiles/deploy.sh`.
- nginx site: `/etc/nginx/sites-available/tiles.waycast.io`
  (HTTP-only until DNS exists).
- Cache seeded with the 470 Hillsboro tiles.
- Verified externally: healthz / single tile / area.tar (3.1 MB in
  1.6 s) via `curl -H "Host: tiles.waycast.io" http://46.4.48.244/...`
- Note: 8140 is firewalled externally; only nginx is public. The
  bastion mangles quoting/args — put commands in scripts, transfer
  via stdin (`cat file | ssh ... "cat > remote"`), then `bash script`.

### LIVE (July 18 2026)
- DNS: apex + www + tiles -> 46.4.48.244 (15 min TTL)
- TLS: one cert for tiles.waycast.io + waycast.io + www.waycast.io
  (certbot --nginx, auto-renews)
- https://waycast.io = landing page (+/index.hu.html,
  /howitworks.html) served from /var/www/waycast
- https://tiles.waycast.io = tile service (healthz/tiles/area.tar
  verified over TLS)
- Device tile_server.h -> https://tiles.waycast.io (OSM fallback
  retained). Site redeploy: rebuild site/, cat files over ssh to
  /var/www/waycast/.
