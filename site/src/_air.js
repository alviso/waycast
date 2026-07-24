/* Waycast landing animation v2 — a stylized town where the mesh's real
 * mechanics play out: cars on streets, geo-ephemeral flooding with
 * contention + suppression, a confirm vote refilling the TTL, and a
 * shop announcing on the LOCAL channel. Click/tap: nearest car reports.
 * Page provides window.AIR_CAPS = [[ms, caption], ...]. */
(function () {
  const cv = document.getElementById('air');
  const cap = document.getElementById('airline');
  const ctx = cv.getContext('2d');
  const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;
  const CAPS = window.AIR_CAPS || [[0, '']];

  /* frame-trace ticker (console lines synced to mesh events) */
  const tk = document.getElementById('airticker');
  function tick(cls, txt) {
    if (!tk) return;
    const d = document.createElement('div');
    d.className = cls; d.textContent = txt;
    tk.appendChild(d);
    while (tk.children.length > 5) tk.removeChild(tk.firstChild);
  }

  const RED = '255,107,107', TEAL = '12,166,120', AMBER = '245,159,0',
        BLUE = '#5C7CFA', ROAD = '#1A2029';
  let W, H;
  function size() {
    const r = cv.getBoundingClientRect(), d = devicePixelRatio || 1;
    W = r.width; H = r.height;
    cv.width = W * d; cv.height = H * d;
    ctx.setTransform(d, 0, 0, d, 0, 0);
  }
  size(); addEventListener('resize', size);

  /* ---- streets: two horizontals (2 lanes each), three verticals ---- */
  const HS = [0.30, 0.66], VS = [0.20, 0.55, 0.84];
  const LANES = [];
  for (const y of HS) {
    LANES.push({ h: 1, f: y, off: -4.5, dir: 1 });
    LANES.push({ h: 1, f: y, off: 4.5, dir: -1 });
  }
  VS.forEach((x, i) => LANES.push({ h: 0, f: x, off: i % 2 ? -4.5 : 4.5,
                                    dir: i % 2 ? -1 : 1 }));

  const cars = [];
  for (let i = 0; i < 16; i++) {
    const lane = LANES[i % LANES.length];
    cars.push({ lane, p: Math.random(),
                id: (0x1000 + Math.floor(Math.random() * 0xEFFF))
                      .toString(16).toUpperCase(),
                v: 0.024 + Math.random() * 0.02,  /* frac of span / s */
                heard: 0, relayAt: 0, relayed: false, supp: false,
                local: 0, fixed: false });
  }
  /* town nodes (anchors) at intersections; shop beacon on main street */
  const towns = [{ fx: VS[0], fy: HS[0] }, { fx: VS[1], fy: HS[1] }]
    .map(t => ({ ...t, heard: 0, relayAt: 0, relayed: false, supp: false,
                 local: 0, fixed: true, town: true }));
  const shop = { fx: 0.70, fy: 0.30, local: 0 };
  const nodes = cars.concat(towns);

  const place = n => {
    if (n.fixed) { n.x = n.fx * W; n.y = n.fy * H; return; }
    const L = n.lane, span = L.h ? W : H, q = n.p * span;
    if (L.h) { n.x = q; n.y = L.f * H + L.off; }
    else { n.x = L.f * W + L.off; n.y = q; }
  };
  const placeShop = () => { shop.x = shop.fx * W; shop.y = shop.fy * H - 14; };

  const RANGE = n => W * (n && n.town ? 0.235 : 0.17);
  const RADIUS = () => W * 0.38;

  /* ---- event timeline ---- */
  const TTL = 24000, FADE = 3000, ATTEST_AT = 11500, LOCAL_AT = 17000,
        NEXT_AT = TTL + FADE + 1800;
  let origin = null, op = null, t0 = -1e9, attested = false;
  let evSeq = 0, expTicked = false;
  const rings = [];   /* {x,y,t0,c(rgb string),haz:bool} */

  function newEvent(now, chosen) {
    for (const n of nodes) {
      n.heard = 0; n.relayAt = 0; n.relayed = false; n.supp = false;
      n.local = 0;
    }
    shop.local = 0; rings.length = 0; attested = false;
    origin = chosen || cars[Math.floor(Math.random() * cars.length)];
    place(origin); op = { x: origin.x, y: origin.y };
    origin.heard = now; origin.relayed = true; origin.hop = 1;
    rings.push({ x: op.x, y: op.y, t0: now, c: RED, haz: true,
                 R: RANGE(origin), hop: 1 });
    t0 = now;
    evSeq++; expTicked = false;
    tick('tx', `[tx ] hazard ${origin.id}/${evSeq} — DEBRIS · 29 B on air`);
  }

  cv.style.cursor = 'pointer';
  cv.addEventListener('click', e => {
    if (reduced) return;
    const r = cv.getBoundingClientRect();
    const mx = e.clientX - r.left, my = e.clientY - r.top;
    let best = null, bd = 1e9;
    for (const c of cars) {
      place(c);
      const d = Math.hypot(c.x - mx, c.y - my);
      if (d < bd) { bd = d; best = c; }
    }
    newEvent(performance.now(), best);
  });

  function step(now) {
    for (const c of cars) {
      c.p += c.v * c.lane.dir * 0.016;
      if (c.p > 1.02) c.p -= 1.04;
      if (c.p < -0.02) c.p += 1.04;
      place(c);
    }
    towns.forEach(place); placeShop();
    const age = now - t0;
    if (age > NEXT_AT) newEvent(now);

    /* hazard flood */
    for (const g of rings) {
      if (!g.haz) continue;
      const rr = (now - g.t0) * 0.14;
      for (const n of nodes) {
        if (n.heard) continue;
        const d = Math.hypot(n.x - g.x, n.y - g.y);
        if (d <= Math.min(rr, g.R)) {
          n.heard = now;
          n.hop = g.hop || 1;
          n.rssi = -Math.round(58 + 38 * Math.min(d / g.R, 1));
          if (Math.hypot(n.x - op.x, n.y - op.y) > RADIUS()) { n.supp = true; continue; }
          n.relayAt = now + 300 + (1 - Math.min(d / RANGE(n), 1)) * 1700;
        }
      }
    }
    for (const n of nodes) {
      if (!n.relayAt || n.relayed || now < n.relayAt) continue;
      n.relayAt = 0;
      let cov = 0;
      for (const g of rings)
        if (g.haz && Math.hypot(n.x - g.x, n.y - g.y) < RANGE(n) * 0.8) cov++;
      if (cov >= 3) {
        n.supp = true;
        tick('sup', `[sup] ${n.town ? 'town-node' : n.id} heard ×${cov} — stays quiet`);
        continue;
      }
      n.relayed = true;
      rings.push({ x: n.x, y: n.y, t0: now, c: RED, haz: true,
                   R: RANGE(n), hop: (n.hop || 1) + 1 });
      tick('rly', `[rly] ${n.town ? 'town-node' : n.id} repeats · hop ${n.hop || 1} · ${n.rssi || -80} dBm`);
    }

    /* confirm vote: an oncoming car inside the radius pings green */
    if (!attested && age >= ATTEST_AT && age < TTL) {
      let best = null, bd = 1e9;
      for (const c of cars) {
        if (!c.heard || c === origin) continue;
        const d = Math.hypot(c.x - op.x, c.y - op.y);
        if (d < RADIUS() && d < bd) { bd = d; best = c; }
      }
      if (best) {
        attested = true;
        rings.push({ x: best.x, y: best.y, t0: now, c: TEAL, haz: false, conf: true, R: W * 0.1 });
        tick('att', `[att] ${best.id} confirms — ttl extended`);
      }
    }

    /* shop announcement on the LOCAL channel */
    if (!shop.local && age >= LOCAL_AT) {
      shop.local = now;
      rings.push({ x: shop.x, y: shop.y, t0: now, c: TEAL, haz: false, R: RANGE(null) });
      tick('loc', '[loc] shop announces → corkboard (LOCAL channel)');
    }
    for (const g of rings) {
      if (g.haz || g.conf) continue;
      const rr = (now - g.t0) * 0.11;
      for (const n of nodes)
        if (!n.local && Math.hypot(n.x - g.x, n.y - g.y) <= Math.min(rr, g.R))
          n.local = now;
    }

    if (!expTicked && age > TTL && origin) {
      expTicked = true;
      tick('exp', `[exp] ${origin.id}/${evSeq} expired — forgotten by design`);
    }

    let line = CAPS[0][1];
    for (const [t, txt] of CAPS) if (age >= t) line = txt;
    if (cap.dataset.c !== line) { cap.dataset.c = line; cap.textContent = line; }
  }

  function drawRoads() {
    ctx.strokeStyle = ROAD; ctx.lineWidth = 10; ctx.lineCap = 'round';
    for (const y of HS) { ctx.beginPath(); ctx.moveTo(8, y * H); ctx.lineTo(W - 8, y * H); ctx.stroke(); }
    for (const x of VS) { ctx.beginPath(); ctx.moveTo(x * W, 8); ctx.lineTo(x * W, H - 8); ctx.stroke(); }
    ctx.strokeStyle = 'rgba(92,102,114,.25)'; ctx.lineWidth = 1; ctx.setLineDash([6, 10]);
    for (const y of HS) { ctx.beginPath(); ctx.moveTo(8, y * H); ctx.lineTo(W - 8, y * H); ctx.stroke(); }
    for (const x of VS) { ctx.beginPath(); ctx.moveTo(x * W, 8); ctx.lineTo(x * W, H - 8); ctx.stroke(); }
    ctx.setLineDash([]);
  }

  function chevron(n, color) {
    const L = n.lane, a = L.h ? (L.dir > 0 ? 0 : Math.PI)
                             : (L.dir > 0 ? Math.PI / 2 : -Math.PI / 2);
    ctx.save(); ctx.translate(n.x, n.y); ctx.rotate(a);
    ctx.fillStyle = color;
    ctx.beginPath(); ctx.moveTo(5, 0); ctx.lineTo(-4, -3.6); ctx.lineTo(-4, 3.6);
    ctx.closePath(); ctx.fill(); ctx.restore();
  }

  function draw(now) {
    ctx.clearRect(0, 0, W, H);
    drawRoads();
    const age = now - t0, fade = Math.max(0, Math.min(1, (TTL + FADE - age) / FADE));
    const live = age < TTL + FADE;

    /* relevance radius */
    if (age > 5000 && fade > 0) {
      ctx.setLineDash([5, 7]);
      ctx.strokeStyle = `rgba(${AMBER},${0.32 * fade * Math.min(1, (age - 5000) / 700)})`;
      ctx.beginPath(); ctx.arc(op.x, op.y, RADIUS(), 0, 7); ctx.stroke();
      ctx.setLineDash([]);
    }

    /* rings with a soft glow */
    for (const g of rings) {
      const speed = g.haz || g.conf ? 0.14 : 0.11;
      const rr = (now - g.t0) * speed, R = g.R;
      if (rr > R) continue;
      const al = (1 - rr / R) * (g.haz ? 0.5 : 0.65);
      ctx.shadowColor = `rgba(${g.c},.8)`; ctx.shadowBlur = 10;
      ctx.strokeStyle = `rgba(${g.c},${al})`; ctx.lineWidth = 1.6;
      ctx.beginPath(); ctx.arc(g.x, g.y, rr, 0, 7); ctx.stroke();
      ctx.shadowBlur = 0;
    }

    /* shop beacon (diamond) */
    ctx.save(); ctx.translate(shop.x, shop.y); ctx.rotate(Math.PI / 4);
    ctx.fillStyle = shop.local && live ? '#0CA678' : 'rgba(12,166,120,.45)';
    ctx.fillRect(-4.5, -4.5, 9, 9); ctx.restore();

    /* nodes */
    for (const n of nodes) {
      /* heard halo / suppressed gray */
      if (n.heard && fade > 0) {
        ctx.fillStyle = n.supp ? `rgba(134,142,150,${0.15 * fade})`
                               : `rgba(${RED},${0.18 * fade})`;
        ctx.beginPath(); ctx.arc(n.x, n.y, 9, 0, 7); ctx.fill();
      }
      /* armed-to-relay amber blink */
      if (n.relayAt && now < n.relayAt && fade > 0 && Math.floor(now / 160) % 2) {
        ctx.strokeStyle = `rgba(${AMBER},.8)`; ctx.lineWidth = 1.4;
        ctx.beginPath(); ctx.arc(n.x, n.y, 7, 0, 7); ctx.stroke();
      }
      if (n.town) {
        ctx.fillStyle = '#0CA678';
        ctx.fillRect(n.x - 4.5, n.y - 4.5, 9, 9);
        ctx.strokeStyle = 'rgba(12,166,120,.4)'; ctx.lineWidth = 1;
        ctx.strokeRect(n.x - 7.5, n.y - 7.5, 15, 15);
      } else {
        chevron(n, n === origin && fade > 0 ? '#FF6B6B' : BLUE);
      }
      /* LOCAL pin */
      if (n.local && live) {
        ctx.fillStyle = 'rgba(12,166,120,.9)';
        ctx.beginPath(); ctx.arc(n.x, n.y - 9, 1.9, 0, 7); ctx.fill();
      }
    }

    /* hazard TTL arc at the origin point */
    if (fade > 0 && op) {
      const eff = attested ? Math.max(0, age - 6000) : age;
      const rem = Math.max(0, 1 - eff / TTL);
      ctx.strokeStyle = `rgba(${RED},${0.9 * fade})`; ctx.lineWidth = 2.2;
      ctx.beginPath();
      ctx.arc(op.x, op.y, 12, -Math.PI / 2, -Math.PI / 2 + rem * Math.PI * 2);
      ctx.stroke();
      ctx.fillStyle = `rgba(${RED},${0.9 * fade})`;
      ctx.beginPath(); ctx.arc(op.x, op.y, 2.6, 0, 7); ctx.fill();
    }
  }

  if (reduced) {
    newEvent(0);
    for (const n of nodes) { place(n); n.heard = 1; }
    placeShop(); shop.local = 1;
    draw(1200); cap.textContent = CAPS[CAPS.length - 1][1];
    [['tx', '[tx ] hazard 3F2A/1 — DEBRIS · 29 B on air'],
     ['rly', '[rly] 8C41 repeats · hop 1 · -71 dBm'],
     ['sup', '[sup] B334 heard ×3 — stays quiet'],
     ['att', '[att] A1C2 confirms — ttl extended'],
     ['exp', '[exp] 3F2A/1 expired — forgotten by design']
    ].forEach(([c, t]) => tick(c, t));
    return;
  }
  let last = 0;
  function loop(now) {
    if (now - last >= 1000 / 30) { last = now; step(now); draw(now); }
    requestAnimationFrame(loop);
  }
  requestAnimationFrame(loop);
})();
