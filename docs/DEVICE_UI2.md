# Device UI v2 — the two-posture town square

Direction ratified July 21 2026. This document is the design source of
truth for the four-pillar UI revision (traffic alerts, local
information, groups, businesses).

## Ratified decisions

1. **Two postures are the organizing principle.** DRIVING = ambient
   radio: the device talks, you don't. PARKED = town square: browsing
   and composing are allowed because attention is safe. Product
   sentence: *while you drive it protects you; when you stop, the town
   opens up.*
2. **Parked is ONE mixed feed** (the corkboard). Channel is expressed
   as card color; tabs (Nearby / Groups / Alerts) are filters over the
   same feed, never separate rooms.
3. **Groups v1 = convoy, not chat.** Keyed group; members appear as
   distinct pins on the map; ~6 canned statuses one-tap while driving;
   free text parked-only and deliberately effortful. Postcards, not
   chat — group traffic must never eat the safety channel.
4. **Passenger affirmation** ("I'm a passenger") unlocks the parked UI
   while moving, Waze-style. The affirmation is a tap, its wording is
   honest, and it is remembered for the trip, not forever.
5. **Phone as input surface: deferred.** BLE-local input wouldn't
   break the off-grid claim, but it's a second codebase. The group
   design must not require it.

## The interrupt ladder (driving leak-through)

| Rank | Channel | While driving |
|---|---|---|
| 1 | SAFETY | Full interrupt: card + chime, always |
| 2 | TRAFFIC | Map pin + NEXT AHEAD card, no chime |
| 3 | GROUP | One chime + sender name line, once; convoy card updates silently |
| 4 | LOCAL / PLACE | Map pins only, never a sound |
| 5 | QUERY / REPLY | Parked-only entirely |

## Card anatomy (the corkboard unit)

Every card in the parked feed shares one skeleton; channels vary only
the accent color and the action row.

```
| (channel color bar)                                    |
| TITLE                      · distance + rough direction |
| provenance · age · expiry                               |
| trust: N confirmed / M doubted   [✓ confirm] [✕ doubt]  |
| [channel-specific canned actions]        [free text…]   |
```

- **Provenance is always shown**, in plain words: "another driver,
  2 hops away" / "town node" / "the place itself" / "official
  (NWS via town node)". Trust lives in provenance + votes, never in
  identity (there are no accounts to trust).
- **Expiry is the soul.** Cards visibly age: opacity ramps down over
  the last ~20% of TTL, and expired cards are *gone* on next paint.
  No archive, no history tab. The product's privacy story is enforced
  by the UI refusing to remember.
- Confirm/doubt buttons appear only where attestation makes sense
  (hazards, local notices) — not on group messages or official feeds.
- Free text everywhere is bounded by the wire's `note[40]` — the
  compose row shows a 40-char budget meter, SMS-style. The budget IS
  the aesthetic: telegrams, not essays.

## Posting flow (parked; evolves today's Report flow)

Target: 3 taps to on-air for the common case.

1. **+ Post** (fixed, bottom-right of the feed)
2. **Channel picker** — four big tiles: Hazard (red) / Local notice
   (teal) / My group (purple) / — Places don't post from cars.
3. **Category tile** (per channel: hazard types; local categories
   lodging/fuel/event/aid/info; group = straight to note)
4. Optional note (40-char meter), then one big **"Put it on the air"**.

Radius + TTL come from category defaults (`VMESH_HZ_INFO` /
local-info table); an "adjust" disclosure exposes them but nobody
should need it. After send: the card appears in your own feed marked
"yours · on air", with the airtime cost shown once ("~0.6 s of air") —
gentle education that air is shared.

## Group ceremony ("the handshake at the trailhead")

Constraint: no cameras (no QR), no servers, no accounts. Proposal:
radio-proximity pairing, PIN-confirmed:

- **Create**: name the group → device generates key + group channel →
  "pairing open for 90 s" → screen shows a 4-digit PIN.
- **Join**: "join a group" → device listens, finds the invite
  broadcast, shows the group name → joiner types the 4-digit PIN read
  off the host's screen → key is delivered encrypted under a
  PIN-derived secret. Both screens show the same 2-word confirmation
  phrase; humans compare aloud.
- Range-limited by physical proximity (parking lot), time-limited by
  the 90 s window, MITM-resisted by the spoken confirmation phrase.
- **Honest threat model, stated in the UI's trust screen**: this
  ceremony protects convoy privacy against casual listeners. It is
  not a cryptographic identity system; a recorder at the trailhead
  with the PIN window open could brute the PIN offline. Acceptable
  for the convoy tier; revisit if groups ever carry more than
  coordination.
- Leaving = forgetting the key. Group keys rotate with the standing
  weekly pseudonym rotation (per PRODUCT_PLAN §7¾) only if all
  members re-derive; v1: keys are stable until the group is deleted.

## Screen map (from today's S1–S5)

| Screen | Status |
|---|---|
| S1 Driving | Kept; + convoy card, + in-context confirm/deny on passed hazard |
| S2 Town square (parked feed) | NEW — replaces the parked LOCAL unfold |
| S3 Post flow | Evolved from the Report sheet (channel step added) |
| S4 Group handshake | NEW |
| S5 Radio disclosure / trust | Kept; + group ceremony threat-model text |
| — Passenger affirmation | New modal, entered from a small "passenger?" affordance |

## Explicitly open

- Phone-as-input (deferred, keep the door open: nothing in the group
  protocol may assume on-device typing).
- Voice for canned statuses while driving (later; the canned-tile UI
  must not require it).
- Whether QUERY/REPLY needs a "pending" indicator when the place is
  slow to answer (radio reality: maybe never answers).
