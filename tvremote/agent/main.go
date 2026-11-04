// tvremote agent: runs on the TV (root), driven over SSH by tools/tvremote on
// a PC. Commands on stdin, one per line:
//
//	frame [N]        capture the screen, scale it down N times (default 2),
//	                 answer "frame <bytes>\n" followed by a JPEG
//	key <code>       press and release a remote key (evdev code)
//	down <code>      press a key; up <code> releases it
//
// Errors are answered with "err <message>\n". The screen comes from the
// compositor's own mirror (tizen_screenshooter / tizen_screenmirror): what the
// graphics plane shows, so TV pictures on the video plane (antenna, HDMI)
// appear black. The mirror only sees composited windows, so while the agent
// captures, the compositor's hardware overlay planes are switched off
// (enlightenment_info -hwc 0) and back on when the agent exits. Keys are
// written to the remote control's input device, the same path as real presses.
package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/jpeg"
	"os"
	"os/exec"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"
)

var le = binary.LittleEndian

const rcDevice = "/dev/input/event0" // the remote control's input device

func main() {
	defer hwc(true)
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM, syscall.SIGHUP)
	go func() { <-sig; hwc(true); os.Exit(0) }()
	in := bufio.NewScanner(os.Stdin)
	out := bufio.NewWriter(os.Stdout)
	var s *screen
	reply := func(format string, a ...any) { fmt.Fprintf(out, format+"\n", a...); out.Flush() }
	for in.Scan() {
		f := strings.Fields(in.Text())
		if len(f) == 0 {
			continue
		}
		switch f[0] {
		case "frame":
			scale := 2
			if len(f) > 1 {
				scale, _ = strconv.Atoi(f[1])
			}
			if s == nil {
				hwc(false)
				var err error
				if s, err = openScreen(); err != nil {
					reply("err %v", err)
					continue
				}
			}
			jpg, err := s.capture(scale)
			if err != nil {
				reply("err %v", err)
				s.close()
				s = nil
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

// downscale turns the captured ARGB8888 (little-endian: B, G, R, A) buffer
// into an RGB image n times smaller, averaging n x n blocks. Fully
// transparent pixels (where the video plane shows through) come out black.
func downscale(pix []byte, w, h, stride, n int) *image.RGBA {
	if n < 1 {
		n = 1
	}
	ow, oh := w/n, h/n
	img := image.NewRGBA(image.Rect(0, 0, ow, oh))
	for y := 0; y < oh; y++ {
		for x := 0; x < ow; x++ {
			var r, g, b int
			for dy := 0; dy < n; dy++ {
				row := pix[(y*n+dy)*stride:]
				for dx := 0; dx < n; dx++ {
					p := row[(x*n+dx)*4:]
					a := int(p[3])
					b += int(p[0]) * a / 255
					g += int(p[1]) * a / 255
					r += int(p[2]) * a / 255
				}
			}
			d := n * n
			o := img.PixOffset(x, y)
			img.Pix[o], img.Pix[o+1], img.Pix[o+2], img.Pix[o+3] = byte(r/d), byte(g/d), byte(b/d), 255
		}
	}
	return img
}

// hwc switches the compositor's hardware overlay planes on or off.
var hwcOff bool

func hwc(on bool) {
	if on == !hwcOff {
		return
	}
	arg := "0"
	if on {
		arg = "1"
	}
	cmd := exec.Command("enlightenment_info", "-hwc", arg)
	cmd.Env = append(os.Environ(), "XDG_RUNTIME_DIR=/run")
	if cmd.Run() == nil {
		hwcOff = !on
	}
}

func encode(img image.Image) ([]byte, error) {
	var buf bytes.Buffer
	err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: 80})
	return buf.Bytes(), err
}
