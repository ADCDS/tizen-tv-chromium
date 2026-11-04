# tvremote agent: static binary for the TV (ARMv7), driven over SSH by ../tools/tvremote.
build/tvremote-agent-armv7: agent/*.go go.mod
	@mkdir -p build
	CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -ldflags='-s -w' -o $@ ./agent

clean:
	rm -rf build

.PHONY: clean
