# wlxbridge: static Go binary for the chroot (ARMv7; runs on the TV's kernel).
build/wlxbridge-armv7: main.go go.mod go.sum
	@mkdir -p build
	CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -ldflags='-s -w' -o $@ .

clean:
	rm -rf build

.PHONY: clean
