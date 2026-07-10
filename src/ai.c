/* ai.c — OpenUG AI module implementation. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "physics.h"   /* PHYS_MAXSPD paces the AI against the player's cap */

int load_circuit(const char *dataroot, const char *circuit, N2Scene *scene,
                 N2Path *aipath, AiCar *ais, float spawn[3],
                 float *heading0, int *start_idx, float cx, float cy) {
    static const float AICOL[N_AI][3] = {
        {0.15f,0.4f,0.95f}, {0.2f,0.8f,0.35f}, {0.95f,0.8f,0.15f}, {0.85f,0.2f,0.8f} };
    free(aipath->xy); aipath->xy = NULL; aipath->n = 0;
    char pathp[1024];
    snprintf(pathp, sizeof pathp, "%s/TRACKS/%s", dataroot, circuit);
    long plen; unsigned char *pdata = n2_read_file(pathp, &plen);
    if (!pdata) return 0;
    int ok = n2_load_path(pdata, plen, aipath) > 4;
    free(pdata);
    if (!ok) { free(aipath->xy); aipath->xy = NULL; aipath->n = 0; return 0; }
    /* Spawn the PLAYER at the densest built-up spot (cx,cy passed in) so the
       opening view frames the city — even if that's off the racing line (the
       lap logic tracks the nearest waypoint, so the race still works). The AI
       grid on the line, nearest that spot. */
    int best = 0; float bestd = 1e30f;
    for (int i = 0; i < aipath->n; i++) {
        float dx = aipath->xy[i*2]-cx, dy = aipath->xy[i*2+1]-cy, dd = dx*dx+dy*dy;
        if (dd < bestd) { bestd = dd; best = i; }
    }
    *start_idx = best;
    spawn[0]=cx; spawn[1]=cy;
    spawn[2]=n2_ground_z(scene, cx, cy, spawn[2]);
    float dwx = aipath->xy[best*2]-cx, dwy = aipath->xy[best*2+1]-cy;
    if (dwx*dwx+dwy*dwy < 9.0f) {                 /* already on the line: face along it */
        int nx = (best+1) % aipath->n;
        *heading0 = atan2f(aipath->xy[nx*2+1]-cy, aipath->xy[nx*2]-cx);
    } else {                                       /* face toward the racing line */
        *heading0 = atan2f(dwy, dwx);
    }
    for (int k = 0; k < N_AI; k++) {
        int t = (*start_idx + 2 + k*2) % aipath->n;
        ais[k].t = t; ais[k].lap = 0; ais[k].prevrel = t;
        ais[k].pos[0]=aipath->xy[t*2]; ais[k].pos[1]=aipath->xy[t*2+1];
        ais[k].pos[2]=spawn[2]; ais[k].head=*heading0;
        ais[k].spd = 3.0f + k*0.12f;
        memcpy(ais[k].col, AICOL[k], sizeof AICOL[k]);
    }
    return N_AI;
}

/* steering lock per tick — AI respects the same turn-rate constraint idea as
 * the player (a real car can't snap its heading) */
#define AI_STEER_LOCK 0.06f

void ai_step(AiCar *ai, int k, const N2Path *aipath, N2Scene *scene,
             int start_idx, int player_prog) {
    float ax=aipath->xy[ai->t*2]-ai->pos[0], ay=aipath->xy[ai->t*2+1]-ai->pos[1];
    if (ax*ax+ay*ay < 36.0f) ai->t = (ai->t+1) % aipath->n;
    float da = atan2f(ay, ax) - ai->head;
    while (da >  3.14159f) da -= 6.28318f;
    while (da < -3.14159f) da += 6.28318f;
    if (da >  AI_STEER_LOCK) da =  AI_STEER_LOCK;
    if (da < -AI_STEER_LOCK) da = -AI_STEER_LOCK;
    ai->head += da;
    /* pace: ease off for the bend ahead (angle between the approach and
       the next segment) and rubber-band mildly toward the player. */
    int nx = (ai->t+1) % aipath->n;
    float ox=aipath->xy[nx*2]-aipath->xy[ai->t*2], oy=aipath->xy[nx*2+1]-aipath->xy[ai->t*2+1];
    float li=sqrtf(ax*ax+ay*ay), lo=sqrtf(ox*ox+oy*oy);
    float cosang = (li>1e-3f && lo>1e-3f) ? (ax*ox+ay*oy)/(li*lo) : 1.0f;
    float corner = 0.4f + 0.6f*(cosang>0?cosang:0);   /* 1 straight, 0.4 sharp */
    int aiprog = ai->lap*aipath->n + ai->prevrel;
    float gap = (float)(player_prog-aiprog)/(aipath->n*0.4f);
    if (gap>1) gap=1; if (gap<-1) gap=-1;             /* + = AI behind -> faster */
    float target = PHYS_MAXSPD*(0.68f + k*0.015f) * corner * (1.0f + 0.18f*gap);
    ai->spd += (target - ai->spd) * 0.06f;
    if (ai->spd < 0.5f) ai->spd = 0.5f;
    ai->pos[0] += cosf(ai->head)*ai->spd;
    ai->pos[1] += sinf(ai->head)*ai->spd;
    float agz = n2_ground_z(scene, ai->pos[0], ai->pos[1], ai->pos[2]);
    ai->pos[2] += (agz - ai->pos[2]) * 0.35f;
    /* lap: count when loop-progress wraps past the start/finish */
    int rel = (n2_nearest_wp(aipath, ai->pos[0], ai->pos[1]) - start_idx
               + aipath->n) % aipath->n;
    if (ai->prevrel > aipath->n*3/4 && rel < aipath->n/4) ai->lap++;
    ai->prevrel = rel;
}
