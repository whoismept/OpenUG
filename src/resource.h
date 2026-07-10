/* resource.h — OpenUG ResourceManager module: file mapping and asset
 * discovery (which tracks / cars / circuits exist on disk). Chunk PARSING
 * stays in nfsu2.h — the ground-truth parser this module feeds. */
#ifndef OPENUG_RESOURCE_H
#define OPENUG_RESOURCE_H

#include "nfsu2.h"

/* mmap a file read-only. Lazy paging: pages fault in only when touched, so a
 * 100MB shared "master" region costs just the bytes actually read (TPK header
 * + the few textures we decode), not a full load. NULL on failure. */
unsigned char *res_map_file(const char *path, long *len);

/* Enumerate selectable tracks: STREAM*.BUN files under troot. Writes up to
 * max names (extension stripped) and sets *sel to the entry matching cur.
 * Returns the count. */
int res_list_tracks(const char *troot, char (*list)[64], int max,
                    const char *cur, int *sel);

/* Enumerate selectable cars: folders under DATAROOT/CARS with a GEOMETRY.BIN,
 * minus the shared part folders (WHEELS, SPOILER, ...). */
int res_list_cars(const char *dataroot, char (*list)[64], int max,
                  const char *cur, int *sel);

/* Enumerate selectable circuits: closed-loop ROUTES* Paths*.bin (first
 * waypoint ~= last) whose centroid lies within the track footprint given by
 * mn/mx (+30% margin), so switching route never flings the cars off the
 * rendered map. Paths are written relative to troot ("ROUTESX/PathsN.bin"). */
int res_list_circuits(const char *troot, char (*list)[256], int max,
                      const float mn[3], const float mx[3]);

#endif
