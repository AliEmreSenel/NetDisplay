BUILD_DIR ?= build
JOBS ?= 2

.PHONY: all configure test site website clean
all: configure
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"
configure:
	cmake -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
		-DNETDISPLAY_BUILD_SERVER=ON -DNETDISPLAY_BUILD_CLIENT=ON
test: all
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure --parallel "$(JOBS)"
site website:
	NETDISPLAY_BUILD_DIR="$(BUILD_DIR)" NETDISPLAY_JOBS="$(JOBS)" ./build-site.sh
clean:
	cmake --build "$(BUILD_DIR)" --target clean
