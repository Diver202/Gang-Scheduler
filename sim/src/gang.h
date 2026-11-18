#pragma once
#include "gang_common.h"
#include <bpf/bpf.h>
#include <unistd.h>

// Opens the map pinned by the gang scheduler. Returns -1 if scx_gang isn't running or we aren't root.
inline int openGangMap() { return bpf_obj_get(GANG_MAP_PATH); }

// Adds the calling thread to the gang.
inline bool joinGang(int mapFd, int gang, int index) {
    unsigned int tid = gettid();
    gang_member member{(unsigned)gang, (unsigned)index};
    return bpf_map_update_elem(mapFd, &tid, &member, BPF_ANY) == 0;
}
