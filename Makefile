DEVICE ?= Cardenza
IDF_TARGET ?= esp32s3
PORT ?=

# Browser simulator (DEVICE=CardenzaWeb): Emscripten from $(EMSDK), page in $(WEB_BUILD)/site.
EMSDK ?= $(HOME)/emsdk
EMCMAKE ?= $(or $(shell command -v emcmake 2>/dev/null),$(EMSDK)/upstream/emscripten/emcmake)
CMAKE ?= cmake
WEB_BUILD ?= build/web
WEB_PORT ?= 8000

ifeq ($(DEVICE),Cardenza)
BUILD := build/Cardenza
DEVICE_PROJECT_DIR := Devices/Cardenza

ifeq ($(OS),Windows_NT)
define RUN_IDF
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "& { Set-Location '$(abspath $(DEVICE_PROJECT_DIR))'; idf.py $(1) }"
endef
else
define RUN_IDF
cd $(DEVICE_PROJECT_DIR) && idf.py $(1)
endef
endif
else ifeq ($(DEVICE),CardenzaSim)
BUILD := build/CardenzaSim
SIM_BUILD_TARGET := CardenzaSimHost
SIM_RUN_EXE := $(BUILD)/Devices/CardenzaSim/CardenzaSimHost
ifeq ($(OS),Windows_NT)
SIM_BUILD_TARGET := CardenzaSimDesktop
SIM_RUN_EXE := $(BUILD)/Devices/CardenzaSim/Debug/CardenzaSimDesktop.exe
endif
else ifeq ($(DEVICE),CardenzaWeb)
BUILD := $(WEB_BUILD)
else
$(error Unsupported DEVICE '$(DEVICE)'. Expected Cardenza, CardenzaSim or CardenzaWeb)
endif

IDF_PORT_ARG := $(if $(PORT),-p '$(PORT)',)
CARDENZA_IDF_ARGS := -B '$(abspath $(BUILD))'

.PHONY: all build stress run set-target flash bootloader-flash app-flash erase monitor flash-monitor clean fullclean validate-architecture firmware firmware-target host cardenza cardenza-firmware cardenza-flash cardenza-monitor cardenza-flash-monitor cardenza-host cardenza-clean cardenza-fullclean web web-serve web-deploy learn hal-check hal-update

all: build

# CardenzaHAL (Devices/Cardenza/CardenzaHAL) is the upstream submodule
# https://github.com/203-Systems/CardenzaHAL (AGENT.md). It changes in its own clone and upstream main; here it
# is a clean consumer copy. hal-check enforces that before every firmware build; hal-update pulls.
# HAL_DEV=<HAL clone> builds against that clone instead (CARDENZA_HAL_DEV), to test a HAL change before it
# is pushed; the submodule is not touched.
HAL_DIR := Devices/Cardenza/CardenzaHAL
ifneq ($(HAL_DEV),)
export CARDENZA_HAL_DEV := $(abspath $(HAL_DEV))
endif
HAL_NO_PUSH := no-push://CardenzaHAL-is-a-read-only-dependency

hal-check:
	@test -e $(HAL_DIR)/.git || git submodule update --init $(HAL_DIR)
	@git -C $(HAL_DIR) config remote.origin.pushurl $(HAL_NO_PUSH)
	@test -z "$$(git -C $(HAL_DIR) status --porcelain)" || { \
	  echo "hal-check: $(HAL_DIR) has local changes. Change the HAL in its own clone and push it upstream (AGENT.md);" >&2; \
	  echo "hal-check: work above it in Devices/Cardenza and restore it: git -C $(HAL_DIR) checkout -- . && git -C $(HAL_DIR) clean -fd" >&2; \
	  git -C $(HAL_DIR) status --short >&2; exit 1; }
	@test -z "$(HAL_DEV)" || echo "hal-check: HAL_DEV=$(HAL_DEV): building against that HAL clone, a test build, never ship it" >&2
	@git -C $(HAL_DIR) merge-base --is-ancestor HEAD origin/main 2>/dev/null || \
	  { git -C $(HAL_DIR) fetch -q origin && git -C $(HAL_DIR) merge-base --is-ancestor HEAD origin/main; } || { \
	  echo "hal-check: $(HAL_DIR) is on a commit that is not on upstream main (a local commit?)." >&2; \
	  echo "hal-check: push the HAL change upstream first, then make hal-update (HAL_DEV=<clone> for a test build)." >&2; exit 1; }

# Pull the latest upstream CardenzaHAL; commit the new submodule pointer in this repository.
hal-update:
	git -C $(HAL_DIR) fetch origin
	git -C $(HAL_DIR) checkout -q --detach origin/main
	@git -C $(HAL_DIR) config remote.origin.pushurl $(HAL_NO_PUSH)
	@echo "CardenzaHAL: $$(git -C $(HAL_DIR) log --oneline -1). Commit the submodule pointer ($(HAL_DIR)) here."

build:
ifeq ($(DEVICE),Cardenza)
	@$(MAKE) --no-print-directory hal-check
	$(call RUN_IDF,$(CARDENZA_IDF_ARGS) build)
else ifeq ($(DEVICE),CardenzaWeb)
	"$(EMCMAKE)" $(CMAKE) -S . -B $(BUILD) -DDEVICE=CardenzaWeb
	$(CMAKE) --build $(BUILD) --target CardenzaWeb
	@echo "CardenzaWeb page: $(BUILD)/site (make web-serve)"
else
	cmake -S . -B $(BUILD) -DDEVICE=$(DEVICE)
	cmake --build $(BUILD) --target $(SIM_BUILD_TARGET) --config Debug
endif

run: build
ifeq ($(DEVICE),Cardenza)
	@echo run is only supported for DEVICE=CardenzaSim or CardenzaWeb
	@exit 1
else ifeq ($(DEVICE),CardenzaWeb)
	$(MAKE) web-serve
else
	"$(abspath $(SIM_RUN_EXE))"
endif

web:
	$(MAKE) DEVICE=CardenzaWeb build

# The Learn site (wiki/, Docusaurus: the guide and the manual) into $(WEB_BUILD)/site/learn. Its
# screenshots, key lights and reference tables come from the host build (CardenzaSimShot,
# CardenzaLearnDump in build/linux-groove), where the guide's lessons run first (tools/wiki/generate.py).
LEARN_HOST_BUILD ?= build/linux-groove
learn:
	$(CMAKE) --build $(LEARN_HOST_BUILD) --target CardenzaSimShot CardenzaLearnDump
	python3 tools/wiki/generate.py --host-build $(LEARN_HOST_BUILD)
	cd wiki && { test -d node_modules || npm ci; } && npm run build && npm run test:search
	rm -rf "$(WEB_BUILD)/site/learn" && mkdir -p "$(WEB_BUILD)/site" && cp -r wiki/build "$(WEB_BUILD)/site/learn"

# Production page on Vercel (https://cardenza-synth.203.io); see tools/web-deploy.sh for the setup.
web-deploy:
	tools/web-deploy.sh

# Any static server works; the page needs no special headers.
web-serve:
	@test -f "$(WEB_BUILD)/site/cardenza.wasm" || { echo "No web build in $(WEB_BUILD)/site: run make web first"; exit 1; }
	@echo "Serving $(WEB_BUILD)/site on http://localhost:$(WEB_PORT)/"
	python3 -m http.server $(WEB_PORT) --bind 127.0.0.1 --directory "$(WEB_BUILD)/site"

set-target:
ifeq ($(DEVICE),Cardenza)
	$(call RUN_IDF,$(CARDENZA_IDF_ARGS) set-target $(IDF_TARGET))
else
	@echo set-target is only supported for DEVICE=Cardenza
	@exit 1
endif

flash bootloader-flash app-flash erase monitor:
ifeq ($(DEVICE),Cardenza)
	$(call RUN_IDF,$(CARDENZA_IDF_ARGS) $(IDF_PORT_ARG) $@)
else
	@echo $@ is only supported for DEVICE=Cardenza
	@exit 1
endif

flash-monitor:
ifeq ($(DEVICE),Cardenza)
	$(call RUN_IDF,$(CARDENZA_IDF_ARGS) $(IDF_PORT_ARG) flash monitor)
else
	@echo flash-monitor is only supported for DEVICE=Cardenza
	@exit 1
endif

# The on-board stress test (Applications/Synth/StressTest.cpp): the
# release configuration plus the task watchdog (a hang becomes a crash report with its backtrace) and
# the task list for the test's stall watcher. Flash it from its build folder (cd build/Cardenza-stress &&
# esptool.py --chip esp32s3 -p <port> write_flash @flash_args) and read the `stress:` lines of the log.
# Never ship this build.
# STRESS_ONLY=card:DEMO-2,sampler (scenario names, comma separated) and STRESS_SECONDS=60 run those alone;
# STRESS_START=3 begins 3 s after boot, STRESS_REPEAT=1 runs a scenario again after a restart
# instead of going on to the next. STRESS_TRACE=worst traces the heap during that
# scenario (internal RAM still held, by caller: `stress: H` lines; the RAM left each second: `stress: M`).
# STRESS_LOOPS=4 runs the list four times over; with STRESS_TRACE the run ends at the first one that got low.
STRESS_BUILD := build/Cardenza-stress
stress:
ifeq ($(DEVICE),Cardenza)
	@$(MAKE) --no-print-directory hal-check
	@mkdir -p $(STRESS_BUILD)
	@sed -e 's/^# CONFIG_ESP_TASK_WDT_PANIC is not set$$/CONFIG_ESP_TASK_WDT_PANIC=y/' \
	     -e 's/^# CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 is not set$$/CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y/' \
	     -e 's/^# CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1 is not set$$/CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y/' \
	     -e 's/^# CONFIG_FREERTOS_USE_TRACE_FACILITY is not set$$/CONFIG_FREERTOS_USE_TRACE_FACILITY=y/' \
	     -e 's/^# CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS is not set$$/CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y/' \
	     $(DEVICE_PROJECT_DIR)/sdkconfig > $(STRESS_BUILD)/sdkconfig.next
	@if [ -n "$(STRESS_TRACE)" ]; then \
	  sed -i -e 's/^CONFIG_HEAP_TRACING_OFF=y$$/# CONFIG_HEAP_TRACING_OFF is not set/' \
	         -e 's/^# CONFIG_HEAP_TRACING_STANDALONE is not set$$/CONFIG_HEAP_TRACING_STANDALONE=y/' $(STRESS_BUILD)/sdkconfig.next; \
	  printf 'CONFIG_HEAP_TRACING_STACK_DEPTH=8\nCONFIG_HEAP_TRACE_HASH_MAP=y\nCONFIG_HEAP_TRACE_HASH_MAP_IN_EXT_RAM=y\nCONFIG_HEAP_TRACE_HASH_MAP_SIZE=1024\n' >> $(STRESS_BUILD)/sdkconfig.next; \
	fi
	@cmp -s $(STRESS_BUILD)/sdkconfig.next $(STRESS_BUILD)/sdkconfig.stress || cp $(STRESS_BUILD)/sdkconfig.next $(STRESS_BUILD)/sdkconfig.stress
	$(call RUN_IDF,-B '$(abspath $(STRESS_BUILD))' -DSDKCONFIG='$(abspath $(STRESS_BUILD))/sdkconfig.stress' -DCARDENZA_STRESS_TEST=ON -DCARDENZA_STRESS_ONLY='$(STRESS_ONLY)' -DCARDENZA_STRESS_SECONDS='$(STRESS_SECONDS)' -DCARDENZA_STRESS_START='$(STRESS_START)' -DCARDENZA_STRESS_REPEAT='$(STRESS_REPEAT)' -DCARDENZA_STRESS_TRACE='$(STRESS_TRACE)' -DCARDENZA_STRESS_LOOPS='$(STRESS_LOOPS)' build)
	@echo "stress firmware in $(STRESS_BUILD): cd $(STRESS_BUILD) && esptool.py --chip esp32s3 -p <port> write_flash @flash_args, then read the stress: lines"
else
	@echo stress is only supported for DEVICE=Cardenza
	@exit 1
endif

host:
	@echo Cardenza hardware HAL is ESP-IDF only. Use DEVICE=CardenzaSim for host validation.
	@exit 1

validate-architecture:
	cmake -DCARDENZA_ROOT="$(abspath .)" -P cmake/ValidateArchitecture.cmake

clean:
ifeq ($(OS),Windows_NT)
	if exist "$(BUILD)" rmdir /s /q "$(BUILD)"
else
	rm -rf $(BUILD)
endif

fullclean:
ifeq ($(DEVICE),Cardenza)
	$(call RUN_IDF,$(CARDENZA_IDF_ARGS) fullclean)
else
	$(MAKE) DEVICE=$(DEVICE) clean
endif

firmware: build
firmware-target: set-target

cardenza:
	$(MAKE) DEVICE=Cardenza build

cardenza-firmware:
	$(MAKE) DEVICE=Cardenza build

cardenza-flash:
	$(MAKE) DEVICE=Cardenza flash PORT=$(PORT)

cardenza-monitor:
	$(MAKE) DEVICE=Cardenza monitor PORT=$(PORT)

cardenza-flash-monitor:
	$(MAKE) DEVICE=Cardenza flash-monitor PORT=$(PORT)

cardenza-host:
	$(MAKE) host

cardenza-clean:
	$(MAKE) DEVICE=Cardenza clean

cardenza-fullclean:
	$(MAKE) DEVICE=Cardenza fullclean
