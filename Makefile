CC ?= gcc
.DEFAULT_GOAL := all
CFLAGS ?= -O3 -std=c11 -Wall -Wextra
LDFLAGS ?=
LDLIBS ?= -pthread

EXPERIMENT_DIR := Src/experiment
BUILD_DIR ?= build/experiments
SOURCES := $(EXPERIMENT_DIR)/main.c \
	$(EXPERIMENT_DIR)/kernels.c \
	$(EXPERIMENT_DIR)/perf.c

ELEMENT_TYPE ?= float
ifeq ($(ELEMENT_TYPE),double)
TYPE_FLAG := -DELEMENT_TYPE_DOUBLE
else ifneq ($(ELEMENT_TYPE),float)
$(error ELEMENT_TYPE must be either float or double)
endif

EXPERIMENTS := \
	$(BUILD_DIR)/reduction \
	$(BUILD_DIR)/memset \
	$(BUILD_DIR)/vector-multiply \
	$(BUILD_DIR)/scalar-multiply

.PHONY: all clean FORCE

FORCE:

all: $(EXPERIMENTS)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/reduction: FORCE $(SOURCES) $(EXPERIMENT_DIR)/inc/kernels.h $(EXPERIMENT_DIR)/inc/perf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TYPE_FLAG) -DDEFINE=REDUCTION $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/memset: FORCE $(SOURCES) $(EXPERIMENT_DIR)/inc/kernels.h $(EXPERIMENT_DIR)/inc/perf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TYPE_FLAG) -DDEFINE=MEMSET $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/vector-multiply: FORCE $(SOURCES) $(EXPERIMENT_DIR)/inc/kernels.h $(EXPERIMENT_DIR)/inc/perf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TYPE_FLAG) -DDEFINE=VECTOR_MULTIPLY $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD_DIR)/scalar-multiply: FORCE $(SOURCES) $(EXPERIMENT_DIR)/inc/kernels.h $(EXPERIMENT_DIR)/inc/perf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TYPE_FLAG) -DDEFINE=SCALAR_MULTIPLY $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

clean:
	rm -rf $(BUILD_DIR)
