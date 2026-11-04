# tvkioskd: static Go binary that runs natively on the TV (ARMv7).
build/tvkioskd-armv7: *.go go.mod go.sum $(wildcard start/*)
	@mkdir -p build
	CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -ldflags='-s -w' -o $@ .

clean:
	rm -rf build

.PHONY: clean
