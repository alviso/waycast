# Vehicular Mesh — Phase 0: Product & UI Plan

**Phase goal:** a working, demoable UI on the ESP32-P4 hardware, driven entirely by
**simulated data**. No LoRa, no GPS, no radio code. This phase validates the two
things not already de-risked by prior Meshtastic experience:

1. The **P4 graphics stack** (MIPI-DSI + PPA + LVGL 9) actually delivers a smooth
   moving-map experience.
2. The **product itself is compelling on screen** — a hazard map that appears, ages,
   and expires feels like something people want on their dash.

Companion document: [handoff-spec.md](handoff-spec.md) (the full project spec).
Section references (§) below point into that spec.

### Explicitly out of scope for Phase 0
- LoRa PHY, mesh forwarding, congestion control (§8) — deferred, *not* foreclosed.
- GPS hardware — position comes from scenario playback.
- BLE phone tether, OTA, Wi-Fi.
- Vector map rendering (raster-first per §6).

---

## 1. Architecture: the feed seam

The single load-bearing decision. The UI never talks to a radio *or* to the
simulator directly — it consumes and publishes messages through one small
interface (`msg/feed.h`):

```
                        ┌──────────────────────┐
  Phase 0:              │  scenario player     │  sim/
  ┌─────────┐  poll()   ├──────────────────────┤
  │   UI    │ ◄──────── │  (later: mesh stack, │
  │ (LVGL)  │ ────────► │   on-P4 or co-MCU,   │
  └─────────┘ publish() │   behind same API)   │
                        └──────────────────────┘
```

- `vmesh_feed_poll()` — UI pulls incoming hazard/position messages each tick.
- `vmesh_feed_publish()` — UI pushes user-reported hazards out.
- `vmesh_pose_get()` — own vehicle position/heading/speed (scenario now, GPS later).

The message struct crossing this seam is the **§7 message model itself** —
the same fields that will later go over the air. Nothing built in Phase 0 is
throwaway; the mesh slots in behind the identical interface (§5's transport
seam, pointed at simulation first).

### Two build targets, one UI codebase
| Target | Purpose | Stack |
|---|---|---|
| `targets/sdl` | Day-to-day UI development on the Mac. Seconds-long iteration loop. | LVGL 9 + SDL2 window (480×800 portrait) |
| `targets/esp32p4` | Reality check: DSI/touch/PPA bring-up, tile blit performance. | ESP-IDF + Espressif P4 BSP + Waveshare examples (§4, §6) |

Rule: all product code lives in `ui/`, `msg/`, `sim/` and is target-agnostic.
Targets contain only display/input glue and a `main()`.

---

## 2. Screen map

### S1 — Map (home screen, 95% of usage)
- Moving map, own vehicle centered (north-up in v0; heading-up is a fast follow).
- **Hazard overlay**: tappable chips, colored by severity, fading with age (see §5 below).
- **Status bar** (top): radio-state indicator (§3 of spec — the trust story made
  visible: `LoRa · BLE` when live, `SIM` badge in simulation), clock, speed.
- **Report FAB** (bottom-right): entry to S2.
- Map stub in v0 scaffold (grid + track ribbon); real raster tiles from SD are milestone M2.

### S2 — Report-hazard flow (driver-safe: two taps, ever)
1. Tap FAB → full-screen grid of **4 large buttons** (Crash / Slowdown / Debris / Weather).
2. Tap type → hazard is published **immediately at current position** with
   type defaults; a 5-second toast offers *Undo*.
- No text entry, no severity picker, no location adjustment while driving.
  Anything richer happens on the phone via BLE, later.

### S3 — Hazard detail (tap a chip)
- Type, severity, distance & bearing from me, age ("reported 4 min ago"),
  time-to-expiry, hop count heard at. Dismiss by tapping outside.

### S4 — Event feed (swipe / button from S1)
- Chronological list of active nearby events. Secondary screen; useful for
  demos and debugging as much as for users.

### S5 — Settings / About
- Radio state (full §3 disclosure: which radios exist, which are on),
  region profile (US915 fixed in v0), units, scenario picker (sim builds only),
  firmware version + link to source (the open-firmware claim, §3).
- Entry to S6 (Wi-Fi & offline maps).

### S6 — Wi-Fi & offline maps (BUILT July 2026)
- Scan + join (LVGL keyboard, creds in NVS, auto-join at boot). Wi-Fi
  is user-enabled infrastructure only — the mesh never depends on it.
- **On-device map download**: fetches z13–16 tiles around the current
  map view straight onto the TF card (throttled, capped, skip-existing)
  — the device is its own companion app; the card never leaves the
  slot. Product path: self-hosted region bundles behind the same seam.
- Backends behind net/provision.h (feed-seam pattern): simulator runs
  fakes (whole flow demoable on the desk), device runs
  esp_wifi_remote/hosted + esp_http_client.

---

## 3. Hazard taxonomy v0

| Type | Icon idea | Default severity | Default expiry | Default radius |
|---|---|---|---|---|
| `crash` | collision | 3 | 30 min | 5 km |
| `slowdown` | queue | 2 | 15 min | 5 km |
| `debris` | box on road | 2 | 60 min | 3 km |
| `pothole` | crater | 1 | 24 h¹ | 2 km |
| `weather` | ice/fog/flood | 2 | 60 min | 8 km |
| `closure` | barrier | 2 | 4 h | 10 km |
| `emergency` | responder activity | 2 | 20 min | 4 km |

Severity: **1 = info** (gray-blue), **2 = caution** (amber), **3 = danger** (red).
User reports (S2) use the type's default severity; over-the-air messages carry it explicitly.

¹ Long-lived infrastructure hazards strain the "ephemeral" model (nobody re-beacons
a pothole at 3 AM). v0 keeps the long expiry and accepts staleness; a re-report
refresh mechanic is a protocol-phase question. Flagged, not solved.

---

## 4. The ephemerality visual language

The soul of the product is that information is *perishable* — the UI must show it:

- **Age → fade**: chip opacity ramps from 100% (fresh) to ~35% at expiry, then removal.
- **Fresh pulse**: chips younger than ~30 s get a subtle pulse ring — "this just happened."
- **No archive, no history.** Expired means gone. This is a feature, and the demo
  script says it out loud.
- **Dedup refresh**: hearing the same hazard again (same origin+seq) resets nothing;
  hearing a *newer report* of the same type nearby is a new chip (conflict/merge
  handling is a protocol-phase question; v0 just shows both).

---

## 5. Scenario file format (the demo script as data)

JSON, loaded by the scenario player (`sim/`). One file = one repeatable demo.

```json
{
  "name": "highway-demo",
  "description": "3-minute I-280 drive: debris ahead, crash appears, both age out",
  "track": [
    { "t": 0,  "lat": 37.3310, "lon": -122.0300 },
    { "t": 30, "lat": 37.3355, "lon": -122.0410 }
  ],
  "events": [
    {
      "t": 15, "type": "debris", "severity": 2,
      "lat": 37.3348, "lon": -122.0395,
      "expiry_s": 600, "radius_m": 3000,
      "origin": "SIM-A3F2", "note": "tire on shoulder"
    }
  ]
}
```

- `track`: timestamped waypoints; player interpolates position/heading/speed
  (this is the fake GPS).
- `events`: hazards "heard from the mesh" at time `t`. Fields deliberately mirror
  the §7 message model — a scenario file is a recorded mesh session, in effect.
- Later free upgrade: record real mesh traffic to this format → replay field drives.

---

## 6. Message model v0 (the §7 seam, UI-facing cut)

```c
typedef struct {
    uint8_t  version;        // protocol evolution (§7)
    uint8_t  msg_type;       // beacon / hazard / traffic / text
    uint8_t  hazard_type;    // taxonomy above
    uint8_t  severity;       // 1..3
    uint32_t origin_id;      // node id  ┐
    uint16_t seq;            //          ┘ → dedup id
    int32_t  lat_e7, lon_e7; // fixed-point 1e-7 deg (§7 compact encoding)
    uint32_t created_s;      // unix time
    uint16_t ttl_s;          // expiry = created + ttl
    uint16_t radius_m_x10;   // relevance radius, 10 m units (655 km max)
    uint8_t  hops;           // hop count when heard
    uint8_t  _reserved[8];   // auth/signature + pseudonym-rotation headroom —
                             // reserved NOW so the trust decisions stay open
} vmesh_msg_t;
```

Wire encoding, signing, and ID-rotation are protocol-phase work; the struct just
keeps the door open. This struct is what crosses feed, BLE, and (packed) the air.

---

## 7. The 3-minute demo script

What the device should be able to show, end of Phase 0, on the P4 panel:

1. **Cold open** — map is already moving, vehicle gliding along the highway.
   Status bar reads `SIM` (on real hw later: `LoRa · BLE`). *"No SIM card, no
   cellular radio — physically. Everything you'll see arrives peer-to-peer."*
2. **~0:20 — debris ahead.** Chip appears 800 m up the road with a fresh-pulse ring.
   Tap it: *"reported 40 s ago by another vehicle, 800 m ahead."*
3. **~1:00 — drive past it.** Chip slides behind, starts to fade. *"Information
   here is perishable — when it stops being true, it disappears. There is no
   database of where you've been."*
4. **~1:30 — report flow.** Two taps: FAB → *Debris*. Toast: *"Reported —
   broadcasting to nearby vehicles."* *"That's the whole interaction, designed
   for a moving car."*
5. **~2:00 — crash appears** further ahead, red, severity 3, feed screen shows both.
6. **~2:45 — the trust close.** Settings screen: radios listed, all local;
   firmware open source. *"An information network that physically cannot
   surveil you and doesn't die with the cell network."*

---

## 7½. Vision note: hyperlocal intelligence, not just hazards (July 2026)

The long arc is bigger than traffic: **the device is the off-grid local
information layer** — hazards while driving, but also local chatter:
events, available accommodation, fuel/water points, road status. The
layer that still works when cell networks don't (post-disaster is the
sharpest version of the §3 trust story: traffic is the everyday
habit-builder, resilience is why it never leaves the car).

Design consequences (pre-wire-format, so decide early):
- **`channel` becomes first-class in the message model** (safety /
  traffic / local-info / text-chat), with a much wider TTL spectrum —
  hazards live minutes, "rooms available" lives hours. *(Done: channel
  byte + LOCAL categories + speed-gated column, July 2026.)*
- **Two consumption modes:** driving = the glanceable info column
  (safety only, chatter never competes with "CRASH 400 m"); parked =
  a browsable local feed. Mode switch should be speed-gated. *(v0 done.)*
- Airtime: chatter is low-frequency/high-TTL — cheap on the channel
  (§8's enemy remains high-frequency position beaconing).

### The "hyperlocal off-grid navigator" roadmap (July 2026)

The product is three legs on one message model:
1. **Safety layer** (built) — the driving screen.
2. **Local intelligence** — what's around me, off-grid. Remaining work:
   - **S6 Browse mode** (parked): full-screen local feed, category
     filters, newest/nearest sort. The LOCAL column is its teaser.
   - **Posting**: on-device = template quick-posts (like the report
     flow: category + canned phrases); free-text composes on the phone
     over BLE later. 
   - **Beacon mode**: a *stationary* device (shop, campsite, aid
     station) re-broadcasts its notice on a slow interval (5–10 min).
     This makes fixed places first-class citizens of the network and
     is the heart of the resilience story. Needs: TTL-refresh
     semantics (origin re-issues with bumped seq; dedup must treat it
     as a refresh, not a duplicate) — a protocol-phase decision.
3. **Navigation-to-things** — "navigator" earned honestly without a
   routing engine: from any notice/hazard detail, **Guide me** = a
   persistent banner with live bearing arrow + distance (straight-line
   guidance on the map). Off-grid and on-road that is genuinely
   enough; true turn-by-turn (vector maps + routing graph) is a v2+
   question and probably phone-assisted.

Positioning in one line: *hazard alerts like Waze, a local bulletin
board like a campground corkboard, and a disaster radio — in one box
with no account, no SIM, and no server.*

### Infrastructure roadmap: oasis-served payloads

Two heavy payloads reach the device over the same **oasis** transport
(home Wi-Fi / phone companion / Anchor hotspot / HaLoWave — optional,
never a dependency), reusing the C6-Wi-Fi + HTTP + cert-bundle stack:

- **Offline map tiles** (built, July 2026) — coverage tiers, on-device
  ring downloads, LAN/companion serving. See `SERVICES.md` and
  `scripts/fetch_area_tiles.py`.
- **OTA firmware updates** (designed, upcoming) — A/B partitions +
  rollback, a signed manifest on the existing `tiles.waycast.io` box,
  update-only-when-parked. **Design + build order:**
  [OTA.md](OTA.md). Gating decision to make early: the partition-table
  redesign (current table is single-`factory`, no OTA slots) — cheap
  now, expensive after devices ship.

## 7¾. Content trust without moderators (July 2026)

There is no central database, no admins, and no way to recruit or
manage a moderator community. This section is the design answer.

### The reframe: ephemerality is the first moderator

Platform moderation nightmares come from **persistence** and
**amplification** — bad content lives forever and spreads virally.
This platform has neither: a false post dies on its own TTL, travels a
bounded radius, and cannot go viral because there is no global graph.
The bar is therefore NOT "eliminate bad content" — it is **"dampen it
faster than it spreads, and make abuse cost more than it's worth,
locally."** Reject any mechanism whose complexity assumes the
social-network-shaped problem.

The working mental model is a **village corkboard plus word of mouth**
— systems that have self-moderated for millennia on three primitives:
identity continuity (the village knows you), locality (a lie travels
only as far as people carry it), and cheap contradiction (anyone can
say "that's not true"). Everything below is those three primitives
translated to LoRa.

### Identity: locally persistent pseudonyms, not accounts

- Each device births its own keypair; `origin_id` = hash of the public
  key; posts are signed. Identity = **continuity, not registration**
  ("the node I've heard all month" is meaningful; nobody issued it).
- **Pseudonyms rotate weekly** *(DECIDED July 2026)*. Long enough
  to accumulate local standing; short enough to bound tracking (the §3
  privacy tension). Reputation ages out exactly like content does —
  philosophically consistent, and it kills "farm one trusted identity,
  then burn it."
  - Consequences for wire v1: fresh keypair per epoch (origin_id
    changes with it); dedup IDs never span an epoch (TTLs are shorter
    than a week, so no continuity problem); trust tables simply age
    entries out — no revocation machinery needed. Epoch boundaries
    should be per-device-random, not synchronized (a global "everyone
    rotates Sunday midnight" event would be both a correlation gift
    and a trust-table cliff).
- No real names, ever. No registration server, ever.

### Votes: corroboration, not social scoring

Thumbs up/down exists, but it means **"confirmed / not there"** (the
Waze model), never "I like this":
- An attestation is a tiny message (~16 B) referencing (origin, seq):
  new `VMESH_MT_ATTEST` message type, confirm/deny bit. Airtime-trivial.
- **A confirmation IS a TTL refresh** — corroborated content lives
  longer and renders bolder; denied content fades early. One mechanism
  serves freshness and truth (couples with beacon-mode refresh
  semantics).
- Aggregation is purely local: each device tallies what it personally
  heard. No consensus, no global state; devices in different places
  may disagree — it's a navigator, not a court.
- UI: age-fade becomes age-and-doubt fade; detail sheet shows
  "3 confirmed / 1 disputed".

### Enforcement: relay refusal, not deletion

"Deplatforming" in a mesh = **neighbors declining to rebroadcast**.
Each device keeps a small local trust table (pseudonym → first-heard
age, corroboration ratio) modulating (a) display prominence and
(b) relay willingness. A source whose posts keep getting denied is
never "banned" — its messages just stop traveling past its own radio
horizon. The lie dies within earshot of the liar; no admin acted.

### Sybil spam: stacked mitigations, cheapest first

Pseudonyms are free to mint, so per-identity limits alone are weak:
1. **Receiver-side rate limiting** — ignore origins over N posts/hour
   per channel. Trivial; catches lazy abuse.
2. **New-pseudonym cold start** — relays carry unknown identities at
   lower priority until they've been heard behaving for a while.
   Sybils start voiceless.
3. **Proof-of-work stamps** (deploy only if real spam appears) —
   Hashcash-style stamp per post: trivial for one honest post,
   expensive at spam scale, works fully offline.

Honest floor: a determined local adversary with an SDR can always
pollute their own radio neighborhood. Their blast radius is one radio
horizon, their content dies in minutes, and physical locality makes
them locatable. Economics, not perfection, is the goal.

### Explicitly out of scope

Global ban lists (requires authority), retroactive deletion
(impossible; contradicts the model), blockchain/consensus mechanisms
(airtime + battery + complexity; already rejected in the spec), any
server-side reporting (there is no server — that's the product).

### Build order

1. **Now (demoable, no radio):** confirm/deny buttons on the detail
   sheet feeding a local tally; doubt-fade rendering.
2. **Wire format v1 (with the mesh):** signed frames (auth trailer —
   size/truncation trade-offs to study; the version byte at offset 0
   exists for exactly this), `VMESH_MT_ATTEST`, receiver-side rate
   limits.
3. **Later, evidence-driven:** pseudonym epochs, cold-start relay
   policy, PoW stamps if spam actually appears.

## 7⅘. The social layer: friends, DMs, presence (design note, July 2026)

High-level design (NOT scheduled — captured so it isn't re-derived).
Extends the §7¾ identity model from two tiers to three.

**Core tension:** public rotation exists to make a person *unlinkable*
week to week, but friendship needs *persistent recognition*. Resolved
with a **two-key identity**:
- **Public rotating pseudonym** — what the mesh at large sees (hazards,
  votes). Disposable, unlinkable, as already decided.
- **Friend-facing stable key** — exchanged ONLY at pairing, never
  broadcast in clear. Friends recognize each other through this private
  relationship, so rotating the public layer doesn't lose friends.

So the identity model becomes three layers:
1. public rotating pseudonym (mesh-at-large, unlinkable)
2. friend-facing stable key (private, shared only via pairing)
3. place keys (persistent, public, physical) — unchanged from §7¾.

**Direct messages** = a normal flooded frame everyone relays but only
the recipient reads: `[rendezvous tag][encrypted payload]`. The tag is
derived from the shared friend key + current epoch — a rotating flag
only the intended friend matches; relays see opaque bytes and never
learn who talks to whom (metadata-private). Inherently spam-proof: an
unknown sender has no valid tag, so strangers are never decrypted. The
friend channel cannot be cold-messaged.

**Location sharing** — default is SILENCE: persons broadcast no
location (unlike place beacons, which are inherently located). Sharing
is a location-carrying DM emitted periodically to a friend for a
bounded window. Ephemerality makes "share for 1 hour" free — no
stop-sharing/revocation protocol needed; you stop emitting and the
last frame TTLs out. There is no location history ANYWHERE to delete,
because there is nowhere to store it. Not privacy-as-policy;
privacy-as-substrate.

**Pairing is deliberately physical** (QR scan / NFC bump / face-to-face
code). The friction bounds the graph to real relationships — no
follower counts, no algorithmic suggestions, no mass-friending.

**The reframe (product insight):** this does NOT compete with WhatsApp
(global, cloud). It answers a question global apps can't: *"which of my
people is physically near me — at this festival, on this road, in this
blackout — right now?"* Local presence, not global telepresence.
Everyone else's identity lives in a datacenter; ours lives on the two
paired devices.

**Open tensions (own them):**
- Forward secrecy: full Signal-style ratchet likely too heavy for 40 B
  frames on embedded — v1 = shared key + rotating tag, accepting that
  key compromise exposes that friendship's history.
- Text DMs > one frame need fragmentation the wire format lacks.
- Works only when friends share a LOCAL mesh — lean into this as the
  defining characteristic, don't paper over it.
- Losing your device loses your friendships (keys are only on-device) —
  consistent with "nothing stored centrally," but a UX reality.

## 7⅞. Power personalities (July 2026)

Measured baseline: the full dev kit (7" panel + P4 + SD, demo running)
draws **≈2.5 W / 0.5 A at 5 V**. That number splits the product into
three power personalities sharing one protocol:

| Mode | Power source | Screen | Radio | Runtime |
|---|---|---|---|---|
| **Car** | USB / lighter port (recommend: always plugged) | always on | always on | indefinite |
| **Pocket** | battery | **wake on relevance** (button, or nearby-hazard alert → 30 s) | always RX; MCU deep-sleeps, radio IRQ wakes it | ~20 mA-class → a week+ on 5000 mAh |
| **Injector** | solar + pack | none | always on | permanent |

Design consequences:
- The screen is ~everything (2.5 W); the radio is ~nothing (SX1262 RX
  ≈ 10–15 mA). So "battery mode" means *screen discipline*, never
  radio discipline — a node whose radio is off is absent from the
  mesh (no receive, no relay, no corkboard).
- Fully-off stays a valid user choice; the protocol already tolerates
  absent nodes (TTL re-announcement catches you up on power-on). But
  the default pocket state is radio-on/screen-off, because every
  pocket node is also everyone else's relay.
- The P4 + 7" kit cannot demonstrate pocket mode (wrong silicon for
  20 mA sleep); a pocket SKU wants C6/S3-class MCU + small/e-paper
  display. Hardware for later — but the protocol must assume
  duty-cycled listeners from day one (flooding already does: it never
  assumes a node heard the first broadcast).
- Power banks that auto-shut-off below ~100 mA will kill a sleeping
  node — pocket SKU needs a real battery, not a bank.

## 8. Milestones

| # | Milestone | Proves |
|---|---|---|
| **M0** | SDL sim: moving map stub + scenario player + hazard chips + report flow | The seam works; demo is scriptable; UI iteration loop is fast |
| **M1** | **P4 bring-up spike**: Waveshare BSP demo → stock LVGL example on the DSI panel | The scary unknown. Do this *early and thin* — meet the dragons in week 1 |
| **M2** | Raster tiles (§6 pipeline: OSM → MBTiles → RGB565 on SD), pan/zoom on both targets | Map perf on P4 (PPA blit, SD throughput) |
| **M3** | Full S1–S5 UI on the P4, scenario-driven — **the 3-minute demo** | Phase 0 exit: product validated on-screen |
| M4 | GPS module in (pose provider swaps from scenario to NMEA) | First hardware seam swap |
| M5 | Mesh stack behind the feed (per §5 A→B decision) | Phase 1 begins |

M0 and M1 are parallel tracks; they share no code beyond `lv_conf.h`.

## 9. Phase-0 risks

- **P4 BSP maturity** (§4 caveat) — newest silicon, LVGL-DSI path less traveled.
  Mitigation: M1 is deliberately tiny and early; Espressif BSP + Waveshare wiki
  only, no LovyanGFX detours.
- **SDL/device divergence** — the sim will lie about performance. Mitigation:
  M2 runs on both targets; perf judgments only ever made on the P4.
- **Tile pipeline yak-shaving** — server-side OSM rendering can eat weeks.
  Mitigation: M0/M1 use the map *stub*; tiles are isolated in M2 and the render
  layer is renderer-agnostic (§6).
- **Product risk the demo itself tests**: does ephemeral, sparse information feel
  *alive* rather than *empty*? The scenario format exists so we can tune density
  and pacing until it does.
