/* SPDX-License-Identifier: GPL-2.0 */
/* Loader for the gang scheduler. Run with sudo; Ctrl-C switches back to the default scheduler. */
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "gang_common.h"
#include "scx_gang.bpf.skel.h"

const char help_fmt[] =
"Gang scheduler (sched_ext).\n"
"\n"
"Usage: %s [-g GANG_MS] [-o OTHER_MS] [-n NR_GANGS] [-v]\n"
"\n"
"  -g GANG_MS   length of each gang's window in ms (default 8)\n"
"  -o OTHER_MS  length of the background window in ms (default 2)\n"
"  -n NR_GANGS  number of gangs, 1-%d (default 1)\n"
"  -v           print libbpf debug messages\n"
"  -h           show this help\n"
"\n"
"Gang threads register in " GANG_MAP_PATH " (the simulation does this with --gang N).\n";

static bool verbose;
static volatile int exit_req;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format, va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int sig)
{
	exit_req = 1;
}

static __u64 read_stat(struct scx_gang *skel, __u32 idx)
{
	int nr_cpus = libbpf_num_possible_cpus();
	__u64 values[nr_cpus], sum = 0;

	if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats), &idx, values) == 0)
		for (int cpu = 0; cpu < nr_cpus; cpu++)
			sum += values[cpu];
	return sum;
}

static int count_gang_threads(struct scx_gang *skel)
{
	int fd = bpf_map__fd(skel->maps.gang_tids), count = 0;
	__u32 key, next;
	__u32 *prev = NULL;

	while (bpf_map_get_next_key(fd, prev, &next) == 0) {
		count++;
		key = next;
		prev = &key;
	}
	return count;
}

int main(int argc, char **argv)
{
	struct scx_gang *skel;
	struct bpf_link *link;
	__u64 gang_ms = 8, other_ms = 2, ecode;
	__u32 nr_gangs = 1;
	int opt;

	while ((opt = getopt(argc, argv, "g:o:n:vh")) != -1) {
		switch (opt) {
		case 'g':
			gang_ms = strtoull(optarg, NULL, 0);
			break;
		case 'o':
			other_ms = strtoull(optarg, NULL, 0);
			break;
		case 'n':
			nr_gangs = strtoul(optarg, NULL, 0);
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]), MAX_GANGS);
			return opt != 'h';
		}
	}
	if (nr_gangs < 1 || nr_gangs > MAX_GANGS || gang_ms < 1 || other_ms < 1) {
		fprintf(stderr, help_fmt, basename(argv[0]), MAX_GANGS);
		return 1;
	}

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	skel = SCX_OPS_OPEN(gang_ops, scx_gang);
	skel->rodata->gang_window_ns = gang_ms * 1000000;
	skel->rodata->other_window_ns = other_ms * 1000000;
	skel->rodata->nr_gangs = nr_gangs;
	skel->rodata->nr_cpus = libbpf_num_possible_cpus();

	SCX_OPS_LOAD(skel, gang_ops, scx_gang, uei);

	unlink(GANG_MAP_PATH);
	if (bpf_map__pin(skel->maps.gang_tids, GANG_MAP_PATH)) {
		fprintf(stderr, "failed to pin %s\n", GANG_MAP_PATH);
		return 1;
	}

	link = SCX_OPS_ATTACH(skel, gang_ops, scx_gang);
	printf("gang scheduler loaded: %u gang(s), gang window %llu ms, others window %llu ms\n", nr_gangs, gang_ms, other_ms);

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		printf("gang threads %d  gang enqueues %llu  other enqueues %llu  window switches %llu\n", count_gang_threads(skel),
		       read_stat(skel, 0), read_stat(skel, 1), (unsigned long long)skel->bss->nr_window_switches);
		fflush(stdout);
		sleep(1);
	}

	bpf_link__destroy(link);
	bpf_map__unpin(skel->maps.gang_tids, GANG_MAP_PATH);
	ecode = UEI_REPORT(skel, uei);
	scx_gang__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
