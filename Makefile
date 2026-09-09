.PHONY: all site clean
all:
	cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j
site:
	./build-site.sh
clean:
	rm -rf build dist
