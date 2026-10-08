# Builds the BPF objects used by run_iavf_xdp_tests.sh.
# Needs clang and libbpf's bpf_helpers.h; set BPF_INC to a dir containing
# bpf/bpf_helpers.h (see README.md for generating it from a kernel tree).
BPF_INC ?= /usr/include
CLANG   ?= clang
SRCS    := $(wildcard bpf/*.bpf.c)
OBJS    := $(SRCS:.bpf.c=.bpf.o)

all: $(OBJS)

bpf/%.bpf.o: bpf/%.bpf.c
	$(CLANG) -O2 -g -target bpf -D__TARGET_ARCH_x86 -I$(BPF_INC) \
		-I/usr/include/x86_64-linux-gnu -c $< -o $@

clean:
	rm -f bpf/*.bpf.o

.PHONY: all clean
