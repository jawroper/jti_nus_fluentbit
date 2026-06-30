# Makefile for jti_nus Fluent Bit C INPUT plugin (protobuf-c decoder)
#
# Usage:
#   make              — compile proto files (if needed) and build .so
#   make install      — install to /usr/local/lib/fluent-bit/
#   make clean        — remove build/ and .so
#   make dispatch     — regenerate jti_dispatch.c/.h from proto files
#
# Prerequisites:
#   sudo apt-get install gcc libmsgpack-dev protobuf-compiler \
#                        protobuf-c-compiler libprotobuf-c-dev

CC          := gcc
CFLAGS      := -shared -fPIC -O2 \
               -Wno-unused-parameter -Wno-missing-field-initializers
LIBS        := -lmsgpackc -lpthread -lprotobuf-c
OUT         := jti_nus_fluentbit.so
INSTALL_DIR := /usr/local/lib/fluent-bit

JTI_PROTO_DIR := by_release/jti/25.4
EVO_PROTO_DIR := by_release/evo/25.4
BUILD_DIR     := build/proto
MERGED_DIR    := $(BUILD_DIR)/merged
SENTINEL      := $(BUILD_DIR)/.compiled

# Locate the google protobuf include directory at parse time.
# Searches common install locations across Debian/Ubuntu variants.
# libprotobuf-dev installs to /usr/include/google/protobuf/
# protobuf-compiler may install to /usr/share/protobuf/
PROTO_INCLUDE := $(shell find /usr/include /usr/local/include /usr/share/protobuf \
                     -name "descriptor.proto" 2>/dev/null \
                     | head -1 \
                     | xargs -I{} dirname {} \
                     | xargs -I{} dirname {})

PLUGIN_SRCS := cmd/plugin/jti_nus.c \
               cmd/plugin/jti_walker.c \
               cmd/plugin/jti_dispatch.c

.PHONY: all install clean dispatch proto-compile

all: proto-compile $(OUT)

proto-compile: $(SENTINEL)

$(SENTINEL):
	@mkdir -p $(MERGED_DIR)/google/protobuf
	@echo "Compiling google proto files (proto include: $(PROTO_INCLUDE))..."
	protoc-c --c_out=$(MERGED_DIR) --proto_path=$(PROTO_INCLUDE) \
	    google/protobuf/descriptor.proto \
	    google/protobuf/any.proto
	@echo "Compiling JTI proto files..."
	@cp $(JTI_PROTO_DIR)/telemetry_top.proto $(EVO_PROTO_DIR)/ 2>/dev/null || true
	@for f in $(JTI_PROTO_DIR)/*.proto; do \
	    protoc-c --c_out=$(MERGED_DIR) \
	        --proto_path=$(JTI_PROTO_DIR) "$$f" 2>/dev/null || true; \
	done
	@echo "Compiling EVO proto files (overwrites JTI where duplicated)..."
	@for f in $(EVO_PROTO_DIR)/*.proto; do \
	    protoc-c --c_out=$(MERGED_DIR) \
	        --proto_path=$(EVO_PROTO_DIR) "$$f" 2>/dev/null || true; \
	done
	@rm -f $(MERGED_DIR)/cosd_oc_evo.pb-c.c $(MERGED_DIR)/cosd_oc_evo.pb-c.h
	@echo "Proto compilation complete — $$(ls $(MERGED_DIR)/*.pb-c.c 2>/dev/null | wc -l) files"
	@touch $@

$(OUT): $(PLUGIN_SRCS) $(SENTINEL)
	@echo "Linking $(OUT)..."
	@python3 scripts/list_needed_protos.py \
	    cmd/plugin/jti_dispatch.c $(MERGED_DIR) > /tmp/jti_needed_protos.txt
	$(CC) $(CFLAGS) -o $@ \
	    $(PLUGIN_SRCS) \
	    $$(cat /tmp/jti_needed_protos.txt) \
	    -I$(MERGED_DIR) -Icmd/plugin \
	    $(LIBS)
	@echo "Built $@ ($$(ls -lh $@ | awk '{print $$5}'))"

dispatch:
	python3 scripts/gen_dispatch.py \
	    --jti $(JTI_PROTO_DIR) \
	    --evo $(EVO_PROTO_DIR) \
	    --out-c cmd/plugin/jti_dispatch.c \
	    --out-h cmd/plugin/jti_dispatch.h

install:
	@test -f $(OUT) || { echo "ERROR: $(OUT) not found — run 'make' first"; exit 1; }
	@mkdir -p $(INSTALL_DIR)
	install -m 755 $(OUT) $(INSTALL_DIR)/$(OUT)
	@echo "Installed to $(INSTALL_DIR)/$(OUT)"

clean:
	rm -f $(OUT)
	rm -rf build/

update-%:
	python3 scripts/gen_dispatch.py \
	    --jti by_release/jti/$* \
	    --evo by_release/evo/$* \
	    --out-c /tmp/jti_dispatch_new.c \
	    --out-h /tmp/jti_dispatch_new.h && \
	echo "Review /tmp/jti_dispatch_new.c then copy to cmd/plugin/ and run: make clean && make"
