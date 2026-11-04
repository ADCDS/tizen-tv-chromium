// tvremote agent: runs on the TV (root), driven over SSH by tools/tvremote on
// a PC. Commands on stdin, one per line:
//
//	frame [N]        capture the screen, scale it down N times (default 2),
//	                 answer "frame <bytes>\n" followed by a JPEG
//	key <code>       press and release a remote key (evdev code)
//	down <code>      press a key; up <code> releases it
//
// Errors are answered with "err <message>\n". The screen is rebuilt from the
// compositor's window dump (capture.go): what the graphics plane shows, so TV
// pictures on the video plane (antenna, HDMI) appear black. Each frame pauses
// the compositor for about 1.5 s, so frames are meant to be taken on demand
// (after key presses), not streamed. Keys are written to the remote control's
// input device, the same path as real presses.
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/jpeg"
	"os"
	"strconv"
	"strings"
	"time"
)

var le = binary.LittleEndian

const rcDevice = "/dev/input/event0" // the remote control's input device

func main() {
	in := bufio.NewScanner(os.Stdin)
	out := bufio.NewWriter(os.Stdout)
	reply := func(format string, a ...any) { fmt.Fprintf(out, format+"\n", a...); out.Flush() }
	for in.Scan() {
		f := strings.Fields(in.Text())
		if len(f) == 0 {
			continue
		}
		switch f[0] {
		case "frame":
			if up := uptime(); up >= 0 && up < 90*time.Second {
				// Booting: memory is tight and the TV's UI is starting; capturing
				// can wait.
				reply("err TV is starting up (%ds since boot)", int(up.Seconds()))
				continue
			}
			scale := 2
			if len(f) > 1 {
				scale, _ = strconv.Atoi(f[1])
			}
			jpg, err := capture(scale)
			if err != nil {
				reply("err %v", err)
				continue
			}
			reply("frame %d", len(jpg))
			out.Write(jpg)
			out.Flush()
		case "key", "down", "up":
			if len(f) < 2 {
				reply("err usage: %s <code>", f[0])
				continue
			}
			code, err := strconv.Atoi(f[1])
			if err != nil || code <= 0 || code > 767 {
				reply("err bad key code %q", f[1])
				continue
			}
			if err := press(uint16(code), f[0]); err != nil {
				reply("err %v", err)
				continue
			}
			reply("ok")
		default:
			reply("err unknown command %q", f[0])
		}
	}
}

// press writes key events (struct input_event, 16 bytes on the TV's 32-bit
// kernel: timeval, type, code, value) to the remote's input device.
func press(code uint16, how string) error {
	f, err := os.OpenFile(rcDevice, os.O_WRONLY, 0)
	if err != nil {
		return err
	}
	defer f.Close()
	ev := func(typ, code uint16, value int32) []byte {
		b := make([]byte, 16)
		le.PutUint16(b[8:], typ)
		le.PutUint16(b[10:], code)
		le.PutUint32(b[12:], uint32(value))
		return b
	}
	syn := ev(0, 0, 0)
	write := func(v int32) error {
		_, err := f.Write(append(ev(1, code, v), syn...))
		return err
	}
	switch how {
	case "down":
		return write(1)
	case "up":
		return write(0)
	}
	if err := write(1); err != nil {
		return err
	}
	time.Sleep(80 * time.Millisecond)
	return write(0)
}

func uptime() time.Duration {
	b, err := os.ReadFile("/proc/uptime")
	if err != nil {
		return -1
	}
	var secs float64
	if _, err := fmt.Sscanf(string(b), "%f", &secs); err != nil {
		return -1
	}
	return time.Duration(secs * float64(time.Second))
}

func encode(img image.Image) ([]byte, error) {
	var buf bytes.Buffer
	err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: 80})
	return buf.Bytes(), err
}
