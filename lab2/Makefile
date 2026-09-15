CC      := gcc
CLANG   := clang-18
ZYDIS   := zydis/Zydis.c
INC     := -I. -Izydis

BINS := test_a_O0 test_a_O3 test_b_O0 test_b_O3 \
        test_a_clang_O0 test_a_clang_O3 test_b_clang_O0 test_b_clang_O3

all: $(BINS)

test_a_O0: test_a.c sigfpe.c $(ZYDIS) ; $(CC)    -O0 -g $(INC) $^ -o $@
test_a_O3: test_a.c sigfpe.c $(ZYDIS) ; $(CC)    -O3 -g $(INC) $^ -o $@
test_b_O0: test_b.c sigfpe.c $(ZYDIS) ; $(CC)    -O0 -g $(INC) $^ -o $@
test_b_O3: test_b.c sigfpe.c $(ZYDIS) ; $(CC)    -O3 -g $(INC) $^ -o $@

test_a_clang_O0: test_a.c sigfpe.c $(ZYDIS) ; $(CLANG) -O0 -g $(INC) $^ -o $@
test_a_clang_O3: test_a.c sigfpe.c $(ZYDIS) ; $(CLANG) -O3 -g $(INC) $^ -o $@
test_b_clang_O0: test_b.c sigfpe.c $(ZYDIS) ; $(CLANG) -O0 -g $(INC) $^ -o $@
test_b_clang_O3: test_b.c sigfpe.c $(ZYDIS) ; $(CLANG) -O3 -g $(INC) $^ -o $@

clean: ; rm -f $(BINS)

dump: all
	@for b in $(BINS); do \
	  echo "================ $$b ================"; \
	  objdump -d -M intel --no-show-raw-insn $$b | sed -n '/<main>:/,/^$$/p'; \
	done