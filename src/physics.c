/* physics.c — OpenUG Physics module implementation. */
#include <math.h>
#include <assert.h>

#include "physics.h"

/* handbrake: rear grip lets go, the lateral velocity survives → drift */
#define HANDBRAKE_GRIP   0.985f
#define REVERSE_SPD_FRAC 0.2f    /* reverse cap ~45 km/h */
#define REVERSE_ACCEL    0.7f    /* reverse thrust vs forward */
#define BRAKE_ACCEL      1.5f    /* braking vs forward thrust: ~10 m/s^2 */
#define COAST_DRAG       0.9994f /* off-throttle engine braking on top of drag */
/* turn authority ramps in by ~35% of top speed then holds — responsive from
 * low speed without getting twitchy/spinny flat out. */
#define TURN_RAMP_FRAC   0.35f
/* ...and eases off toward top speed so 200+ km/h stays stable (NFSU2 cars
 * corner tight at city speed, wide at full tilt) */
#define TURN_HISPD_DROP  0.55f

float phys_car_step(float pos[3], float vel[2], float *heading, float *speed,
                    float throttle, float steer, int handbrake) {
    float hf[2] = { cosf(*heading), sinf(*heading) };
    float fwd = vel[0]*hf[0] + vel[1]*hf[1];   /* signed forward speed */
    if (throttle > 0) {
        /* throttle tapers as speed builds: punchy off the line, eases near top */
        float sp = *speed < 0 ? -*speed : *speed;
        float a = PHYS_ACCEL * (1.15f - 0.55f*sp/PHYS_MAXSPD) * throttle;
        vel[0] += hf[0]*a; vel[1] += hf[1]*a;
    } else if (throttle < 0) {
        /* moving forward = brakes (strong); at rest / rolling back = reverse */
        float a = PHYS_ACCEL * (fwd > 0.01f ? BRAKE_ACCEL : REVERSE_ACCEL);
        vel[0] += hf[0]*a*throttle; vel[1] += hf[1]*a*throttle;
    } else {
        vel[0] *= COAST_DRAG; vel[1] *= COAST_DRAG;
    }
    vel[0] *= PHYS_FRICTION; vel[1] *= PHYS_FRICTION;
    float spd = sqrtf(vel[0]*vel[0]+vel[1]*vel[1]);
    float dir = (vel[0]*hf[0]+vel[1]*hf[1]) < 0 ? -1.f : 1.f;  /* fwd vs reverse */
    float sfac = spd/(PHYS_MAXSPD*TURN_RAMP_FRAC); if (sfac > 1) sfac = 1;
    float hifrac = spd/PHYS_MAXSPD; if (hifrac > 1) hifrac = 1;
    *heading += steer * PHYS_TURN * sfac * (1.0f - TURN_HISPD_DROP*hifrac) * dir;
    /* decompose velocity in the new heading frame, clamp forward, scrub side */
    float nf[2] = { cosf(*heading), sinf(*heading) }, nr[2] = { nf[1], -nf[0] };
    float vf = vel[0]*nf[0]+vel[1]*nf[1], vl = vel[0]*nr[0]+vel[1]*nr[1];
    if (vf >  PHYS_MAXSPD) vf =  PHYS_MAXSPD;
    if (vf < -PHYS_MAXSPD*REVERSE_SPD_FRAC) vf = -PHYS_MAXSPD*REVERSE_SPD_FRAC;
    vl *= handbrake ? HANDBRAKE_GRIP : PHYS_GRIP;
    vel[0] = nf[0]*vf + nr[0]*vl; vel[1] = nf[1]*vf + nr[1]*vl;
    *speed = vf;                      /* forward speed, for HUD/collision */
    pos[0] += vel[0]; pos[1] += vel[1];
    return vl < 0 ? -vl : vl;         /* drift magnitude */
}

void phys_selftest(void) {
    /* the NFSU2 tuning targets, asserted: 0-100 in 3-6 s, top ~220 km/h,
       100-0 braking well under 5 s */
    float pos[3]={0,0,0}, vel[2]={0,0}, h=0, spd=0;
    int t100 = -1;
    for (int t = 1; t <= 60*60; t++) {
        phys_car_step(pos, vel, &h, &spd, 1.0f, 0, 0);
        if (t100 < 0 && PHYS_KMH(spd) >= 100.0f) t100 = t;
    }
    assert(t100 > 2*60 && t100 < 6*60);
    assert(PHYS_KMH(spd) > 200.0f && PHYS_KMH(spd) < 232.0f);
    /* brake from ~100 km/h */
    vel[0] = cosf(h)*100.0f/3.6f/PHYS_TICKRATE; vel[1] = sinf(h)*100.0f/3.6f/PHYS_TICKRATE;
    int tstop = -1;
    for (int t = 1; t <= 8*60 && tstop < 0; t++) {
        phys_car_step(pos, vel, &h, &spd, -1.0f, 0, 0);
        if (spd <= 0.0f) tstop = t;
    }
    assert(tstop > 0 && tstop < 5*60);
}

int collide_walls(float *pos, float *vel, const float obst[][4], int nobst, float r) {
    int hits = 0;
    for (int o = 0; o < nobst; o++) {
        float x0=obst[o][0]-r, y0=obst[o][1]-r, x1=obst[o][2]+r, y1=obst[o][3]+r;
        if (pos[0]<=x0 || pos[0]>=x1 || pos[1]<=y0 || pos[1]>=y1) continue;
        float pl=pos[0]-x0, pr=x1-pos[0], pd=pos[1]-y0, pu=y1-pos[1], m=pl; int ax=0;
        if (pr<m){m=pr;ax=1;} if (pd<m){m=pd;ax=2;} if (pu<m){m=pu;ax=3;}
        if      (ax==0){ pos[0]=x0; if(vel[0]>0)vel[0]=0; }
        else if (ax==1){ pos[0]=x1; if(vel[0]<0)vel[0]=0; }
        else if (ax==2){ pos[1]=y0; if(vel[1]>0)vel[1]=0; }
        else           { pos[1]=y1; if(vel[1]<0)vel[1]=0; }
        hits++;
    }
    return hits;
}
void collide_walls_selftest(void) {
    float obst[1][4] = {{0,0,10,10}};
    float p[3]={5,5,0}, v[2]={1,1};
    assert(collide_walls(p, v, obst, 1, 1.0f) == 1);       /* deep inside -> resolved */
    assert(p[0]<=0 || p[0]>=10 || p[1]<=0 || p[1]>=10);    /* ...and now outside the box */
    float p2[3]={0.5f,5,0}, v2[2]={2,0};                   /* near left face, moving +x */
    collide_walls(p2, v2, obst, 1, 1.0f);
    assert(p2[0] <= -1.0f + 1e-4f && v2[0] == 0.0f);       /* pushed left, +x vel killed */
    float p3[3]={100,100,0}, v3[2]={1,0};
    assert(collide_walls(p3, v3, obst, 1, 1.0f) == 0);     /* far outside -> untouched */
}

#define WALL_MIN_HEIGHT 2.5f    /* z-extent below this = flat prop, not a wall */
#define WALL_MAX_SPAN   300.0f  /* skip oversized shells (sky domes etc.) */

int phys_collect_walls(const N2Scene *s, float (*obst)[4], int max) {
    int nobst = 0;
    for (int i = 0; i < s->count && nobst < max; i++) {
        if (s->meshes[i].cat != N2_OTHER || s->meshes[i].nverts < 3) continue;
        float ox0=1e30f,oy0=1e30f,oz0=1e30f, ox1=-1e30f,oy1=-1e30f,oz1=-1e30f;
        for (int v=0;v<s->meshes[i].nverts;v++){ float *p=s->meshes[i].verts+v*5;
            if(p[0]<ox0)ox0=p[0]; if(p[0]>ox1)ox1=p[0];
            if(p[1]<oy0)oy0=p[1]; if(p[1]>oy1)oy1=p[1];
            if(p[2]<oz0)oz0=p[2]; if(p[2]>oz1)oz1=p[2]; }
        if (oz1-oz0 < WALL_MIN_HEIGHT) continue;             /* flat: not a wall */
        if (ox1-ox0 > WALL_MAX_SPAN || oy1-oy0 > WALL_MAX_SPAN) continue;
        obst[nobst][0]=ox0; obst[nobst][1]=oy0; obst[nobst][2]=ox1; obst[nobst][3]=oy1;
        nobst++;
    }
    return nobst;
}

#define CAR_RADIUS 2.6f   /* car-to-car collision circle */

float phys_car_contacts(float carpos[3], float vel[2], float speed,
                        AiCar *ais, int nai) {
    const float MIN = CAR_RADIUS*2.0f;
    float thud = 0.0f;
    /* player vs AI: player pushed at full weight; AIs share the rest so they
       don't get shoved off their line too hard. */
    for (int k = 0; k < nai; k++) {
        float dx = ais[k].pos[0]-carpos[0], dy = ais[k].pos[1]-carpos[1];
        float d2 = dx*dx+dy*dy;
        if (d2 > 1e-4f && d2 < MIN*MIN) {
            float d = sqrtf(d2), push = (MIN - d);
            float ux = dx/d, uy = dy/d;
            carpos[0]    -= ux*push*0.5f; carpos[1]    -= uy*push*0.5f;
            ais[k].pos[0]+= ux*push*0.5f; ais[k].pos[1]+= uy*push*0.5f;
            vel[0]*=0.85f; vel[1]*=0.85f;   /* bump scrubs a little speed */
            float s = (speed<0?-speed:speed)/PHYS_MAXSPD;
            if (0.3f + s*0.5f > thud) thud = 0.3f + s*0.5f;
        }
    }
    for (int a = 0; a < nai; a++) for (int b = a+1; b < nai; b++) {
        float dx = ais[b].pos[0]-ais[a].pos[0], dy = ais[b].pos[1]-ais[a].pos[1];
        float d2 = dx*dx+dy*dy;
        if (d2 > 1e-4f && d2 < MIN*MIN) {
            float d = sqrtf(d2), push = (MIN - d)*0.5f, ux = dx/d, uy = dy/d;
            ais[a].pos[0]-=ux*push; ais[a].pos[1]-=uy*push;
            ais[b].pos[0]+=ux*push; ais[b].pos[1]+=uy*push;
        }
    }
    return thud;
}
