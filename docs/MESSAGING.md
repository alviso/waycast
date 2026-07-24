# Waycast messaging — key statements

Source insight (Peter, Meshtastic meetups): what excites ordinary
people is not the technology — it's the **feeling**: off-grid, not
watched, not bending to a big company's will. The pitch must serve
that feeling, and (Waycast twist) every claim must be **checkable** —
the S5 trust screen is the messaging, running on the device.

## The one-liner

> **No towers. No servers. No accounts. Cars talking to cars —
> and nothing else.**

Alternates by context:
- "The road network that isn't on the internet."
- "Your neighborhood, off the grid."
- (HU market: "Az út beszél. Senki sem hallgatózik.")

## Three pillars

### 1. Provably off-grid
> There is no modem inside. Not "we don't upload your data" —
> there is physically **no path to the cloud**. Open the case and
> check. The screen shows every radio the device has and what it's
> doing, any time you tap it.

(Radical difference from every app: not a privacy *policy*, a
privacy *architecture*. The S5 screen is this pillar shipped.)

### 2. Nothing is remembered
> Messages fade like tail lights. A hazard lives for minutes, a
> garage sale for a day — then it's gone everywhere, because there
> is nowhere for it to be stored. No history, no profile, no feed
> to mine, no archive to subpoena.

(Ephemerality reframed from limitation to feature: the mesh forgets
you by design. This is the answer to "but what about spam/stalking"
too — content trust doc §7¾.)

### 3. Nobody owns it
> Open firmware, open hardware, unlicensed spectrum. No company can
> change the rules, raise the price, push an update you didn't ask
> for, or turn it off. If we disappear tomorrow, every device keeps
> working — forever.

(The Meshtastic feeling, stated as a guarantee. Monetization stays
compatible: we sell radios and beacons, never access.)

## Supporting lines (site sections, talks)

- "Every car is a courier. Traffic **is** the infrastructure."
- "Works when everything else doesn't — storm, outage, dead zone.
  The mesh doesn't know the grid exists."
- "Radio-honest: what you hear is what's actually near you. No
  algorithm decides what you see — distance does."
- "The farthest listener repeats first — news travels outward,
  fast, without echo." (the NetworkSim3 mechanic, now real in
  mesh/vmesh_mesh.c)
- "Your shop on the network? Buy an antenna, not an ad." (services
  track)

## Tone rules

- Every claim must be verifiable on the device or in the repo. If
  we can't show it, we don't say it.
- Never punch at named competitors; "the cloud" is the foil.
- Plain words. "No servers" beats "decentralized". "It forgets"
  beats "ephemeral". Save protocol vocabulary for docs.

## Visualization (main page)

Peter's old NetworkSim3 video showed a distance-based mesh where the
weakest/farthest receiver repeats first — people got it visually.
Plan: self-contained canvas animation on the landing page showing
the REAL algorithm: a hazard ripples car-to-car (farthest-first
relay, duplicate suppression), stops at its relevance radius, then
**fades to nothing** (ephemerality made visible). Prototype:
site/src/howitworks.html.
