/* resource.c — OpenUG ResourceManager module implementation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "resource.h"

unsigned char *res_map_file(const char *path, long *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return NULL; }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return NULL;
    *len = (long)st.st_size;
    return (unsigned char *)p;
}

int res_list_tracks(const char *troot, char (*list)[64], int max,
                    const char *cur, int *sel) {
    int n = 0;
    DIR *td = opendir(troot); struct dirent *de;
    while (td && n < max && (de = readdir(td))) {
        char *dot = strstr(de->d_name, ".BUN");
        if (strncmp(de->d_name, "STREAM", 6) || !dot) continue;
        int L = (int)(dot - de->d_name); if (L > 63) L = 63;
        memcpy(list[n], de->d_name, L); list[n][L] = 0;
        if (!strcmp(list[n], cur)) *sel = n;
        n++;
    }
    if (td) closedir(td);
    return n;
}

int res_list_cars(const char *dataroot, char (*list)[64], int max,
                  const char *cur, int *sel) {
    static const char *parts[] = { "AUDIO","BRAKES","EXHAUST","PLATES","ROOF",
        "WHEELS","SPOILER","SPOILER_HATCH","SPOILER_SUV","MIRRORS_BODY",
        "MIRRORS_HUMMER","MIRRORS_POST","MIRRORS_SUV" };
    int n = 0;
    char croot[1024]; snprintf(croot, sizeof croot, "%s/CARS", dataroot);
    DIR *cd = opendir(croot); struct dirent *de;
    while (cd && n < max && (de = readdir(cd))) {
        if (de->d_name[0] == '.') continue;
        int isp = 0;
        for (unsigned p = 0; p < sizeof parts/sizeof parts[0]; p++)
            if (!strcmp(de->d_name, parts[p])) isp = 1;
        if (isp) continue;
        char gp[1200]; snprintf(gp, sizeof gp, "%s/%s/GEOMETRY.BIN", croot, de->d_name);
        FILE *f = fopen(gp, "rb"); if (!f) continue; fclose(f);
        int L = (int)strlen(de->d_name); if (L > 63) L = 63;
        memcpy(list[n], de->d_name, L); list[n][L] = 0;
        if (!strcmp(list[n], cur)) *sel = n;
        n++;
    }
    if (cd) closedir(cd);
    return n;
}

int res_list_circuits(const char *troot, char (*list)[256], int max,
                      const float mn[3], const float mx[3]) {
    int n = 0;
    DIR *td = opendir(troot); struct dirent *de;
    while (td && n < max && (de = readdir(td))) {
        if (strncmp(de->d_name, "ROUTES", 6)) continue;
        char rdir[1200]; snprintf(rdir, sizeof rdir, "%s/%s", troot, de->d_name);
        DIR *rd = opendir(rdir); struct dirent *re;
        while (rd && n < max && (re = readdir(rd))) {
            if (strncmp(re->d_name, "Paths", 5)) continue;
            char rel[256]; snprintf(rel, sizeof rel, "%s/%s", de->d_name, re->d_name);
            int dup=0; for (int i=0;i<n;i++) if(!strcmp(list[i],rel)) dup=1;
            if (dup) continue;
            char pp[1500]; snprintf(pp, sizeof pp, "%s/%s", troot, rel);
            long pl; unsigned char *pd = n2_read_file(pp, &pl); N2Path tp = {0};
            if (pd && n2_load_path(pd, pl, &tp) > 4) {
                /* closed loop (start ~= end) whose centroid sits in this
                   track's footprint (+30% margin, since a circuit may
                   overhang the mesh bbox). Regions are far apart, so the
                   centroid test keeps other regions' loops off the list. */
                float dx=tp.xy[0]-tp.xy[(tp.n-1)*2], dy=tp.xy[1]-tp.xy[(tp.n-1)*2+1];
                float px=0,py=0; for(int i=0;i<tp.n;i++){ px+=tp.xy[i*2]; py+=tp.xy[i*2+1]; }
                px/=tp.n; py/=tp.n;
                float ex=(mx[0]-mn[0])*0.3f, ey=(mx[1]-mn[1])*0.3f;
                if (dx*dx+dy*dy < 900.0f &&
                    px>=mn[0]-ex && px<=mx[0]+ex && py>=mn[1]-ey && py<=mx[1]+ey)
                    strncpy(list[n++], rel, 255);
            }
            free(tp.xy); free(pd);
        }
        if (rd) closedir(rd);
    }
    if (td) closedir(td);
    return n;
}
