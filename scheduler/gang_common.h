#pragma once

// Shared between the BPF scheduler, its loader and the simulation.

#define GANG_MAP_PATH "/sys/fs/bpf/gang_tids"
#define MAX_GANGS 4

// Value stored in the gang_tids map, keyed by thread id.
struct gang_member {
    unsigned int gang;   // which gang (0 .. nr_gangs-1)
    unsigned int index;  // worker index inside the gang; its home CPU is index % nr_cpus
};
