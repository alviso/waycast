# Services over the mesh — brainstorm track (opened July 2026)

*A living document, deliberately separate from the device roadmap.
Question on the table: what SERVICES can exist on a P2P local
broadcast mesh — and could you genuinely shop at a local store that
exists only on this network?*

## The medium, stated honestly

Every service idea must survive these physics:
- **Broadcast, not pipes.** Everyone in earshot hears everything.
  Request/response is possible (address a frame to an origin_id) but
  costs double airtime and zero privacy.
- **Tiny + slow.** 29–69 B frames, ~10–20 frames/sec of TOTAL channel
  capacity shared by everyone nearby, seconds of latency, no delivery
  guarantee.
- **Geo-scoped + ephemeral.** Radius and TTL are first-class; nothing
  persists anywhere central.
- **Pseudonymous.** Weekly-rotating identities (§7¾) — with one big
  consequence explored below.

The design instinct that fits: **announcement-shaped beats
conversation-shaped.** The mesh is a corkboard, a town crier, a CB
channel — not a phone line. Historical proof that constrained local
media host thriving service economies: classified ads, CB channel 19,
Minitel, packet-radio BBSs, LETS local currencies, Daknet's data
mules. None had broadband. All had commerce.

## Tier 0 — Stationary injectors (sensors & signage)

The beacon-mode roadmap item, grown into a service class. A fixed,
powered node is cheap (a Pi + bonnet today) and its data is exactly
mesh-shaped: tiny, perishable, hyperlocal.

- **Micro-weather**: road-surface temp, ice, wind gusts on the pass,
  fog line. A chain of $50 stations gives drivers what no cloud
  service has: conditions on THIS road THIS minute.
- **Parking**: a lot beacons its free-space count. Perfect payload
  (2 bytes), perfect perishability.
- **Queue state**: the barbershop/border-crossing/clinic broadcasts
  its wait time.
- **Fuel/charging**: price + stalls-free.
- **Transit**: the bus itself beacons "route 23, 5 min late" as it
  drives — a moving injector.
- **Civic**: school-zone-active flag, burn-ban status, water point,
  shelter open/full (the §7½ resilience set).

Business observation: **injectors are signage.** Merchants already pay
for signs; a beacon is a sign that reaches every dash in 3 km. That is
who funds the stationary infrastructure — no token needed, the edges
monetize in meatspace.

### Tier 0 hardened (July 17 2026 discussion)

**The RAK2287 concentrators CAN inject.** SX1302 has a real TX chain
(LoRaWAN downlink path; lgw_send in libloragw) — peer/vmesh_peer.c is
one config away from being an injector daemon. Constraints: one TX at
a time, RX-blind while transmitting. Superpower: a concentrator hears
ALL SFs on 8 channels simultaneously — half-blind as a talker,
omniscient as a listener. Fixed nodes get the fancy silicon; vehicles
don't need it. Peter's crankk fleet (~10k community, deployed,
powered, elevated) is a ready-made seed injector network.

**Composition = captive portal on the injector.** The Pi hosts a
local Wi-Fi SSID + dumb form (category, 40-char note, duration).
Shopkeeper connects with any phone; the message radiates from their
own roof. No app, no account, no internet, no server of ours.
Off-grid purity holds because content originates AT the place it
describes — which is also a spam defense: a claim about a place,
transmitted from that place, reaching only its actual neighborhood.

**Beacon mode is the one protocol addition** (backlog item, now with
a spec sketch): place-posts want heartbeat semantics — re-emit every
N minutes, TTL refreshed, same origin, bumped seq, receivers treat as
refresh not duplicate (attestation TTL-refresh machinery is most of
this). Cadence caps per category (lodging 1/10min, event 1/2min
while active) keep airtime honest; relay-refusal punishes violators.

**The adoption ladder starts at the laptop (July 18).** Peter's
call: in practice a Windows PC / Mac + USB LoRa dongle is the v0
composer AND injector — the machine the shop already owns, with a
real keyboard, running `tools/composer` (built): beacons a LOCAL
notice on schedule and answers QUERYs from command-line-supplied
knowledge. Zero-commitment trial ($25 dongle); the always-on beacon
box (Pi / SX1262 appliance, roof antenna, solar) is the UPGRADE for
convinced shops, with the desktop app becoming its editor. Same wire
format at every rung. Concentrators stay network-side (super-listener
relays), not shop-side.

**Monetization stance: sell the antenna, not the ad.** Selling
airtime/placement makes us an operator and collapses the no-servers
trust story into a slow ad network. Instead: a shop buys a **place
beacon** (Pi+concentrator today; a ~$40 solar SX1262 box as the
product) that makes it exist on the network. One-time hardware,
Meshtastic economics, aligned incentives — every beacon densifies the
mesh, which makes devices more useful, which sells devices. Spam
control is the content-trust layer + physics (40B notes, geo-radius),
not a billing department.

## Tier 1 — Matching (broadcast + tiny handshake)

- Classifieds/corkboard (LOCAL channel today): selling, seeking,
  lost-dog, tool lending ("have generator / need chainsaw" — gold
  after a storm).
- **Ride matching**: "heading to town, 2 seats, dep 8:30" — the match
  itself is 2–3 frames.
- Taxi/pickup hail: broadcast need, drivers answer.

## Tier 2 — Commerce: the shop question

Decompose shopping: **discovery → order → payment → fulfillment.**

- **Discovery** — solved. A catalog is a set of Tier-0 notices
  ("fresh bread until 18:00 — $4 — 6 left"). Slow-cadence beacons.
- **Order** — fits! A *reservation* is 1–2 frames: "hold 2, pseudonym
  X, pickup 17:30" → store ACKs. This is not e-commerce with carts;
  it's the village model: *call ahead and they set it aside.*
- **Payment** — the honest ladder:
  1. **v1: pay at pickup.** The mesh does discovery + reservation;
     meatspace does money. This is ALREADY a complete, valuable
     service — and it's how small-town commerce worked forever.
  2. **v2: tabs & local credit.** "Put it on my tab" — per-merchant
     IOU ledgers with small limits, backed by pseudonym reputation.
     LETS systems ran whole local economies this way pre-internet.
  3. **v3 (research): offline bearer tokens.** Chaumian e-cash fits
     in frames technically; double-spend control needs an eventual
     reconciler (a credit union as the mint?). Parked — note that
     this is the one rung where "no server" bends.
- **Fulfillment** — physical and local. That's the whole point.

**So: yes, you can shop at a mesh-only store.** v1 = discover +
reserve + pay on pickup. And "mesh-only" is not a limitation — it's
the flywheel: **mesh-exclusive offers give merchants a reason to run
beacons, and merchant beacons give drivers a reason to own devices.**
The daily "mesh special" at the bakery is infrastructure marketing
that pays for itself.

### The identity consequence (design decision surfaced)

Commerce breaks weekly pseudonym rotation — a shop NEEDS a stable
identity (and wants one), and tabs need continuity. Resolution that
keeps §7¾ intact: **two-tier identity — persons rotate, PLACES
persist.** A place key is long-lived, publicly known, tied to a
physical location; a person's rotating pseudonym is enough for
reservations (the bread doesn't care who you were last week; the tab
does, and opening a tab = opting into continuity with that one
merchant). This slots cleanly into beacon mode and the trust layer:
place keys are also natural anchors for the web of trust.

## Tier 2½ — The interface problem: agents, not pages (July 18 2026)

Peter's insight: web-style browsing (menu → categories → photos) is
physically impossible on 40-byte frames — and the replacement is not
a smaller webpage, it's a different interface species: **an agent at
the beacon IS the business's interface.**

- **Agents as bandwidth compressors.** The menu is 10 KB; the answer
  ("Anything vegan under $15?") is 38 chars. The agent converts messy
  business knowledge into exactly the bytes the channel affords. An
  LLM makes a 40-byte pipe feel rich — a framing nobody else needs,
  because nobody else builds on a 40-byte pipe.
- **The agent lives at the place** (the beacon box, fed via the
  captive portal). Knowledge stays local, off-grid, shop-owned. A
  Pi-class board runs a quantized 1–3B model well enough for
  two-line answers.
- **v0 needs no AI**: quick-ask chips (hours? wait? specials?)
  answered from structured portal fields — same wire shape, agent
  drops in later for freeform questions.
- **Wire**: QUERY (to a place key, ≤40 B) + REPLY (refs query, 1–2
  frames). A conversation = 2–6 frames; browsing would be 50+.
- **UX**: parked-only (matches LOCAL browse gating). Tap place pin →
  quick-asks; keyboard only at rest. 40-char answers as a format:
  "Pho $12 · wait 10m · open til 9" — haiku commerce.
- **Trust is architectural**: replies signed by the persistent place
  key (§7¾); agent answers ONLY from shopkeeper-entered facts with
  canned fallback — it cannot invent a discount; worst failure is
  "ask at the counter."
- Limits owned honestly: seconds of latency (fine parked), capped
  exchange count, per-beacon reply budgets in the existing
  rate-limit family.
- Sim-first prototype path: a scripted "place agent" in the scenario
  player can demo the whole interaction before any radio exists.

## Tier 3 — Places as communication infrastructure

A powered, stationary, storage-rich node can offer:
- **Mailbox / "leave word at the inn"**: store-and-forward messages
  addressed to a pseudonym, held at a known place. Async
  person-to-person without any routing — you check your mailbox when
  you pass. The village post office, rebuilt.
- **Trailhead guestbook**: hikers log intentions; SAR checks it.
- **Town polls**: the attest machinery generalizes to straw votes.
- **RTK/DGPS corrections**: a surveyed base station broadcasting GNSS
  corrections — centimeter positioning as a local service.
- **Time beacon** for clock-less nodes.

## Tier 4 — Network services (the mesh helping itself)

- **Bridges**: two mesh islands joined by a directional point-to-point
  link — hobbyist backbone, packet-radio style.
- **Data mules**: buses and mail trucks as scheduled carriers between
  towns (Daknet precedent) — delay-tolerant networking where the
  vehicle IS the internet. Our carry/re-announce §8 work item is the
  primitive this needs.

## What this asks of the protocol (someday, not now)

1. Two-tier identity: persistent place keys alongside rotating
   personal pseudonyms.
2. Addressed frames (to_origin) for reservations/mailboxes —
   unicast-over-broadcast, airtime-priced accordingly.
3. Multi-frame notices (catalog = N chained notices, or simple
   fragmentation) — only if single frames prove too tight.
4. Store-and-forward semantics at stationary nodes.
5. Service discovery = LOCAL channel categories extended (a SERVICE
   category with a service-type byte).
6. Airtime etiquette per class: catalogs slow-cadence, reservations
   bursty-rare, sensors periodic-tiny.

## Parking lot (ideas to revisit)

- Mesh-only auctions (farm box, day-end bakery clearing) — broadcast
  bids are airtime-heavy; maybe sealed-bid single-frame.
- Reputation portability between merchants (probably: don't).
- Emergency mode: services degrade gracefully into the resilience set
  (the bakery beacon becomes the bread-line beacon).
