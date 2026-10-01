LLVM ?= 0
DEBUG ?= 0
ASAN ?= 0
TSAN ?= 0
V ?= 0

ifeq ($(V),0)
Q := @
else
Q :=
endif

ifeq ($(LLVM),0)
CC = $(CROSS_COMPILE)gcc
AR = $(CROSS_COMPILE)ar
LD = $(CROSS_COMPILE)ld
else
CC = clang
AR = llvm-ar
LD = ld.lld
endif

RSEQ_DIR  := librseq
RSEQ_INC  := $(RSEQ_DIR)/include

CFLAGS    ?= -O2 -g -Wall -Wextra
CFLAGS    += -std=gnu11 -pthread -I include -I$(RSEQ_DIR)/include
LDFLAGS   += -pthread

ifneq ($(DEBUG),0)
CFLAGS += -ggdb -DDEBUG
endif

ifneq ($(ASAN),0)
CFLAGS  += -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined -static-libasan
else
ifneq ($(TSAN),0)
CFLAGS  += -fsanitize=thread
LDFLAGS += -fsanitize=thread
endif
endif

RSEQ_OBJS  := $(RSEQ_DIR)/src/rseq.o $(RSEQ_DIR)/src/smp.o
PCREF_OBJS := src/pcref.o
ALL_OBJS   := $(RSEQ_OBJS) $(PCREF_OBJS)
OBJS_DEPS  := $(ALL_OBJS:.o=.d)

TEST_SRCS      := $(wildcard tests/test_*.c)
TEST_OBJS      := $(TEST_SRCS:.c=.o)
TEST_OBJS_DEPS := $(TEST_OBJS:.o=.d)
TESTS          := $(TEST_OBJS:.o=)
RUN_TESTS      := $(addprefix run-,$(TESTS))

STATIC := libpcref.a

.PHONY: clean fmt tests run-tests submodule-check

$(STATIC): $(ALL_OBJS)
	$(info AR      $@)
	$(Q)$(AR) rcs $@ $^

%.o: %.c
	$(info CC      $@)
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(RSEQ_DIR)/src/%.o: $(RSEQ_DIR)/src/%.c | submodule-check
	$(info CC-DEP  $@)
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(OBJS_DEPS)

tests: $(TESTS)

tests/test_%.o: tests/test_%.c
	$(info CC-TEST $@)
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

tests/test_%: tests/test_%.o $(STATIC)
	$(info LD-TEST $@)
	$(Q)$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

-include $(TEST_OBJS_DEPS)

run-tests/%: tests/%
	$(Q)./$< >/dev/null && \
		echo "TEST    $< OK" || \
		{ echo "TEST    $< FAIL"; exit 1; }

run-tests: $(RUN_TESTS)

submodule-check:
	@test -f $(RSEQ_DIR)/include/rseq/rseq.h || { \
		echo "librseq submodule missing; run: git submodule update --init --recursive"; \
		exit 1; }

fmt:
	$(Q)find src/ tests/ -name "*.c" | xargs -I{} clang-format -i {}
	$(Q)find include/ tests/ -name "*.h" | xargs -I{} clang-format -i {}

clean:
	rm -f $(STATIC) $(ALL_OBJS)
	rm -f $(TESTS) $(TEST_OBJS)
	rm -f $(OBJS_DEPS) $(TEST_OBJS_DEPS)
