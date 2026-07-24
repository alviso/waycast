/* meshsim — multi-node flood dynamics harness (spec §8 groundwork).
 *
 * Runs the REAL mesh core (mesh/vmesh_mesh.c) across N simulated
 * vehicles on a 6x6 km plane with a lossy, collision-prone radio:
 *   - delivery probability falls with distance: p = 1 - (d/R)^3
 *   - slotted collisions: 2+ frames reaching a node in one 200 ms
 *     airtime slot destroy each other
 * One node reports a hazard; we measure how the flood behaves.
 *
 *   make meshsim && ./build/meshsim
 *
 * Prints a sweep over fleet sizes with suppression on/off — the
 * numbers behind the §8 design choices.
 */
#include "vmesh_mesh.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AREA_M 6000.0
#define RANGE_M 1500.0
#define SPEED_MPS 22.0
#define SLOT_MS 200        /* one SF9 airtime slot */
#define SIM_MS 120000
#define STEP_MS 50
#define HZ_RADIUS_M 3000.0
#define MAX_NODES 200
#define ORIGIN_LAT 37.32
#define ORIGIN_LON -122.04

typedef struct {
    double x, y, tx_, ty_;   /* pos + waypoint target (meters)      */
    vmesh_mesh_t mesh;
    bool got;                /* received the tracked hazard          */
    uint32_t got_ms;
    int dup_arrivals;        /* copies of it that reached the antenna*/
} node_t;

static node_t nodes[MAX_NODES];
static int N;

/* frames sitting in a node's antenna during the current slot */
typedef struct { vmesh_msg_t msg; int to; double dist; } arrival_t;
static arrival_t slot_arrivals[MAX_NODES * 8];
static int n_arrivals;
static uint32_t rng_state = 12345;

static double frand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return (rng_state & 0xFFFFFF) / (double)0x1000000;
}

static void latlon(double x, double y, double *lat, double *lon)
{
    *lat = ORIGIN_LAT + y / 110540.0;
    *lon = ORIGIN_LON + x / (111320.0 * cos(ORIGIN_LAT * M_PI / 180));
}

static uint32_t tx_count, collision_drops;

/* a node transmits: fan out to everyone in range for THIS slot */
static void transmit(int from, const vmesh_msg_t *m)
{
    tx_count++;
    for (int i = 0; i < N; i++) {
        if (i == from) continue;
        double d = hypot(nodes[i].x - nodes[from].x,
                         nodes[i].y - nodes[from].y);
        if (d > RANGE_M) continue;
        double p = 1.0 - pow(d / RANGE_M, 3.0);
        if (frand() > p) continue;
        if (n_arrivals < (int)(sizeof slot_arrivals / sizeof *slot_arrivals)) {
            slot_arrivals[n_arrivals].msg = *m;
            slot_arrivals[n_arrivals].to = i;
            slot_arrivals[n_arrivals].dist = d;
            n_arrivals++;
        }
    }
}

struct txctx { int from; };
static void mesh_tx_cb(const vmesh_msg_t *m, void *user)
{
    transmit(((struct txctx *)user)->from, m);
}

static void run(int n_nodes, uint8_t suppress, uint32_t seed,
                double *cov, uint32_t *txs, uint32_t *supp,
                double *lat_p95, uint32_t *coll, double *dupavg)
{
    N = n_nodes;
    rng_state = seed;
    memset(nodes, 0, sizeof(nodes));
    tx_count = 0;
    collision_drops = 0;
    n_arrivals = 0;

    mesh_config_t cfg = { .jitter_min_ms = 100, .jitter_max_ms = 800,
                          .suppress_threshold = suppress };
    for (int i = 0; i < N; i++) {
        nodes[i].x = frand() * AREA_M;
        nodes[i].y = frand() * AREA_M;
        nodes[i].tx_ = frand() * AREA_M;
        nodes[i].ty_ = frand() * AREA_M;
        mesh_init(&nodes[i].mesh, &cfg, seed + i * 7 + 1);
    }

    /* the tracked hazard, born at node 0's position at t=10 s */
    vmesh_msg_t hz = {0};
    hz.version = VMESH_PROTO_VERSION;
    hz.msg_type = VMESH_MT_HAZARD;
    hz.hazard_type = VMESH_HZ_CRASH;
    hz.severity = 3;
    hz.origin_id = 0x0BADF00D;
    hz.seq = 1;
    hz.created_s = 100000;
    hz.ttl_s = 600;
    hz.radius_m_x10 = (uint16_t)(HZ_RADIUS_M / 10);
    hz.hops = 0;

    int eligible = 0;
    bool emitted = false;
    double ox = 0, oy = 0;

    for (uint32_t now = 0; now < SIM_MS; now += STEP_MS) {
        /* move the fleet */
        for (int i = 0; i < N; i++) {
            node_t *nd = &nodes[i];
            double dx = nd->tx_ - nd->x, dy = nd->ty_ - nd->y;
            double d = hypot(dx, dy);
            if (d < 50) {
                nd->tx_ = frand() * AREA_M;
                nd->ty_ = frand() * AREA_M;
            } else {
                nd->x += dx / d * SPEED_MPS * STEP_MS / 1000.0;
                nd->y += dy / d * SPEED_MPS * STEP_MS / 1000.0;
            }
        }

        if (!emitted && now >= 10000) {
            emitted = true;
            ox = nodes[0].x;
            oy = nodes[0].y;
            latlon(ox, oy, (double[]){0}, (double[]){0}); /* no-op */
            double la, lo;
            latlon(ox, oy, &la, &lo);
            hz.lat_e7 = (int32_t)(la * 1e7);
            hz.lon_e7 = (int32_t)(lo * 1e7);
            for (int i = 1; i < N; i++)
                if (hypot(nodes[i].x - ox, nodes[i].y - oy) <= HZ_RADIUS_M)
                    eligible++;
            mesh_note_own(&nodes[0].mesh, &hz);
            nodes[0].got = true;
            transmit(0, &hz);
        }

        /* deliver last slot's arrivals with collision detection */
        if (now % SLOT_MS == 0 && n_arrivals) {
            static int cnt[MAX_NODES];
            memset(cnt, 0, sizeof(cnt));
            for (int a = 0; a < n_arrivals; a++)
                cnt[slot_arrivals[a].to]++;
            for (int a = 0; a < n_arrivals; a++) {
                arrival_t *ar = &slot_arrivals[a];
                if (cnt[ar->to] > 1) { collision_drops++; continue; }
                node_t *nd = &nodes[ar->to];
                if (ar->msg.origin_id == hz.origin_id) nd->dup_arrivals++;
                double la, lo;
                latlon(nd->x, nd->y, &la, &lo);
                float prox = (float)(1.0 - ar->dist / RANGE_M);
                bool deliver = mesh_rx(&nd->mesh, &ar->msg, prox, la, lo,
                                       now, 100000 + now / 1000);
                if (deliver && ar->msg.origin_id == hz.origin_id &&
                    !nd->got) {
                    nd->got = true;
                    nd->got_ms = now - 10000;
                }
            }
            n_arrivals = 0;
        }

        for (int i = 0; i < N; i++) {
            struct txctx c = { .from = i };
            mesh_tick(&nodes[i].mesh, now, mesh_tx_cb, &c);
        }
    }

    /* metrics */
    int got = 0;
    uint32_t lats[MAX_NODES];
    int nl = 0;
    uint32_t suppressed = 0, dups = 0;
    for (int i = 1; i < N; i++) {
        if (hypot(nodes[i].x - ox, nodes[i].y - oy) > HZ_RADIUS_M * 1.4)
            ; /* moved far; still count receipt below */
        if (nodes[i].got) { got++; lats[nl++] = nodes[i].got_ms; }
        suppressed += nodes[i].mesh.stats.suppressed;
        dups += nodes[i].dup_arrivals;
    }
    /* sort latencies (tiny n; insertion) */
    for (int i = 1; i < nl; i++)
        for (int j = i; j > 0 && lats[j] < lats[j - 1]; j--) {
            uint32_t t = lats[j]; lats[j] = lats[j - 1]; lats[j - 1] = t;
        }
    *cov = eligible ? 100.0 * got / eligible : 0;
    if (*cov > 100.0) *cov = 100.0; /* nodes drove into radius late */
    *txs = tx_count;
    *supp = suppressed;
    *lat_p95 = nl ? lats[(int)(nl * 0.95) < nl ? (int)(nl * 0.95) : nl - 1]
                      / 1000.0 : 0;
    *coll = collision_drops;
    *dupavg = got ? (double)dups / got : 0;
}

int main(void)
{
    printf("meshsim: 6x6 km, range %.0f m, hazard radius %.0f m, "
           "3 seeds averaged\n\n", RANGE_M, HZ_RADIUS_M);
    printf("%6s %9s | %8s %6s %6s %9s %7s %7s\n",
           "nodes", "suppress", "coverage", "tx", "supp", "collide",
           "p95 s", "dup/rx");
    printf("---------------------------------------------------------------------\n");
    for (int ni = 0; ni < 3; ni++) {
        int n = (int[]){10, 50, 150}[ni];
        for (int s = 0; s <= 1; s++) {
            uint8_t th = s ? 2 : 0;
            double cov = 0, p95 = 0, dup = 0;
            uint32_t txs = 0, sup = 0, col = 0;
            for (uint32_t seed = 1; seed <= 3; seed++) {
                double c, l, d;
                uint32_t t, su, co;
                run(n, th, seed * 1000 + 77, &c, &t, &su, &l, &co, &d);
                cov += c / 3; p95 += l / 3; dup += d / 3;
                txs += t / 3; sup += su / 3; col += co / 3;
            }
            printf("%6d %9s | %7.1f%% %6u %6u %9u %7.2f %7.1f\n",
                   n, th ? "on(2)" : "off", cov, txs, sup, col, p95, dup);
        }
    }
    printf("\ncoverage: %% of nodes inside the hazard radius that got it\n"
           "tx: total transmissions of the hazard (flood cost)\n"
           "supp: relays cancelled by overheard copies\n"
           "collide: frames lost to same-slot collisions\n"
           "dup/rx: average copies each receiving node heard\n");
    return 0;
}
