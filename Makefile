BUILD_DIR ?= build
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
CMAKE_ARGS ?=

.PHONY: all configure test site clean
all: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"
configure:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
		-DNETDISPLAY_BUILD_SERVER=ON -DNETDISPLAY_BUILD_CLIENT=ON $(CMAKE_ARGS)
test: all
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure --parallel "$(JOBS)"
site:
	NETDISPLAY_BUILD_DIR="$(BUILD_DIR)" NETDISPLAY_JOBS="$(JOBS)" ./build-site.sh
clean:
	cmake --build "$(BUILD_DIR)" --target clean
