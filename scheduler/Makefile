# Builds the gang scheduler. Uses relative paths only, because the project
# folder ("VBD Sim") contains a space.

SCX = ../scx-c-examples/scheds
BPF_INCLUDES = -I$(SCX)/include -I$(SCX)/include/bpf-compat -I$(SCX)/include/lib \
               -I$(SCX)/vmlinux -I$(SCX)/vmlinux/arch/arm64 -I.
BPF_CFLAGS = -g -O2 -Wall -Wno-compare-distinct-pointer-types -D__TARGET_ARCH_arm64 -mcpu=v3 -mlittle-endian
CFLAGS = -std=gnu11 -O2 -Wall -I$(SCX)/include -I$(SCX)/vmlinux -Ibuild -I.

build/scx_gang: scx_gang.c build/scx_gang.bpf.skel.h gang_common.h
	cc $(CFLAGS) scx_gang.c -o $@ -lbpf -lelf -lz -lzstd

build/scx_gang.bpf.skel.h: build/scx_gang.bpf.o
	bpftool gen skeleton $< name scx_gang > $@

build/scx_gang.bpf.o: scx_gang.bpf.c gang_common.h
	@mkdir -p build
	clang $(BPF_CFLAGS) -target bpf $(BPF_INCLUDES) -c $< -o $@

clean:
	rm -rf build

.PHONY: clean
