/* physics.h — OpenUG Physics module: arcade car kinematics (heading-frame
 * velocity with lateral tyre scrub → drift), wall AABB collision, and
 * car-to-car circle separation. No GL, no SDL, no file IO. */
#ifndef OPENUG_PHYSICS_H
#define OPENUG_PHYSICS_H

#include "nfsu2.h"
#include "ai.h"

/* driving constants — car length axis = local X; world +Z up */
#define PHYS_ACCEL    0.3f     /* throttle impulse per tick */
#define PHYS_MAXSPD   4.5f     /* forward speed cap (world units/tick) */
#define PHYS_FRICTION 0.95f    /* per-tick velocity decay */
#define PHYS_TURN     0.045f   /* steering rate at full authority */
#define PHYS_GRIP     0.86f    /* lateral scrub per tick (lower = grippier) */

/* One tick of car kinematics: throttle in [-1..1] along the heading, steering
 * in [-1..1] rotates it, tyres scrub the sideways velocity (handbrake keeps
 * it, so the car slides), position integrates. Returns the post-grip lateral
 * speed magnitude — the drift signal for skid marks / smoke / screech. */
float phys_car_step(float pos[3], float vel[2], float *heading, float *speed,
                    float throttle, float steer, int handbrake);

/* Push (pos.xy) out of any wall AABB (expanded by r) it penetrates, along the
 * least-penetration axis; zero the into-wall velocity so the car slides along
 * the face. Returns the number of walls resolved. */
int collide_walls(float *pos, float *vel, const float obst[][4], int nobst, float r);
void collide_walls_selftest(void);

/* Collect building collision footprints: the 2D (XY) bounding box of every
 * tall N2_OTHER mesh. Flat props/signs/road paint are skipped so they don't
 * block the road. Returns the number of AABBs written. */
int phys_collect_walls(const N2Scene *s, float (*obst)[4], int max);

/* Circle-separate the player from each AI and the AIs from each other.
 * Player is pushed at half weight each way; a bump scrubs a little player
 * speed. Returns the collision thud amplitude for the audio (0 = no hit). */
float phys_car_contacts(float carpos[3], float vel[2], float speed,
                        AiCar *ais, int nai);

#endif
