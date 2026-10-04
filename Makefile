all:
	meson setup build --buildtype=release 2>/dev/null || true
	ninja -C build

clean:
	rm -rf build
