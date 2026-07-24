# waycast.io — site structure plan (July 2026)

Studied meshtastic.org's information architecture (not to copy it —
to learn what a community mesh project's audience needs to find).
Principle from Peter: **show only the muscle we have; add sections as
real content lands; never inflate.**

## What meshtastic.org actually covers (distilled)

Their site answers, in order of user journey:

1. **Identity** (home): one-sentence what-it-is, social proof
   (device count, contributors, regions), a taste of the experience
   (live message demo), three key capabilities, client apps, vendor
   ecosystem. CTAs: Get Started / Docs / Flasher.
2. **Orientation** (docs/about): what it is, what it is NOT (not ham
   radio, no license needed), who it's for, FAQ, how the mesh works.
3. **The path to a working node** (getting started): pick device →
   cable/drivers → flash → configure. ~30 min, decision-tree style.
4. **Hardware catalog**: organized BY VENDOR, then product line;
   specs tables (MCU, radio, GPS, BT), community recommendations,
   peripherals (antennas, GPS, enclosures), DIY.
5. **Software catalog**: per-client (Android / Apple / Web / CLI),
   tools (site planner, simulator), integrations.
6. **Configuration reference**: deep settings docs (radio, channels,
   modules).
7. **Downloads + web flasher**: apps per platform, firmware.
8. **Community**: local/regional groups, community apps, enclosures,
   the social-channel constellation.
9. **Blog** (progress/news) and **Legal** (licensing, trademark).

Key structural insight: their organizing axes are *vendor* (hardware)
and *client platform* (software), because their product is "firmware
that runs on many things."

## Waycast's different axis

Waycast's product is a **network with roles**. Our natural axis is
the tier, not the vendor:

- **Drive** — the car device (P4 navigator today)
- **Anchor** — the town node (Pi + RAK2287 appliance today)
- **Announce** — the shop beacon / composer (Mac/PC + dongle today)

Organizing by role tells our story, avoids echoing their structure,
and every role already has working hardware behind it.

## Structure NOW (everything below has real content today)

1. **Home** — identity sentence, demo video (exists), three-tier
   diagram, honest status strip ("field-proven: device ↔ town node
   bidirectional OTA, July 2026" — real milestones instead of
   adoption stats we don't have).
2. **How it works** — mesh animation (exists) + three-tier
   architecture + the trust design we're proud of: ephemerality as
   moderation, weekly pseudonym rotation, attestations, provably
   off-grid. Source: docs/handoff-spec.md + PRODUCT_PLAN §7¾.
3. **Hardware** — "Reference configurations we run today", one page
   per role: parts list, photos, what it does, status. The town node
   runs on widely available LoRa concentrator hardware (second life
   for existing LoRaWAN gateways — no community named for now).
4. **Engineering journal** — build-log blog. Seed posts exist in
   abundance (first OTA mesh, the RAK2287 resurrection, NWS alerts
   on-air, GPS-disciplined time). This is the "keep adding" vehicle —
   cheapest honest signal of momentum.
5. **Roadmap** — the four product pillars (traffic alerts, local
   information, group communication, business communication), what
   works today vs. what's designed vs. what's exploratory.

## Sections to ADD when their trigger fires (not before)

| Section | Trigger |
|---|---|
| Getting started / flashing guide | firmware repo goes public + first release |
| Downloads | same |
| Software (per-client pages) | a second user-facing client exists |
| Configuration reference | settings surface stabilizes |
| Community (groups, chat) | deliberate opening to the seed community |
| Vendors / shop | first-party hardware phase |
| Legal/trademark | open-source release gate |

## Anti-inflation rules

- No section ships with placeholder text, "coming soon", or empty
  tables.
- Home shows *milestones achieved* (dated, specific), never
  projected numbers.
- The journal is the only place for forward-looking talk, clearly
  framed as such.
