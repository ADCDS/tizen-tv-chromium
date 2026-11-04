package main

import (
	"image"
	"image/draw"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
)

// The screen mirror leaves out some of the TV's own UI windows (the home bar,
// org.volt.firstscreen). When one of those is on screen, its buffer is taken
// from the compositor's window dump (slow: the dump writes every window, about
// 3 s) and drawn over the mirrored frame.
var mirrorMisses = []string{"org.volt.firstscreen"}

type window struct {
	id, title  string
	x, y, w, h int
	shown      bool // mapped, not iconified, not fully obscured
}

// windows lists the top-level windows, topmost first (enlightenment_info -topvwins).
func windows() []window {
	out, err := exec.Command("enlightenment_info", "-topvwins").Output()
	if err != nil {
		return nil
	}
	var ws []window
	for _, l := range strings.Split(string(out), "\n") {
		f := strings.Fields(l)
		// No Win_ID RcsID PID w h x y [Foc] Dep Opaq Visi Icon Map Frame PL@ZPos Parent Title...
		if len(f) < 17 || !strings.HasPrefix(f[1], "0x") {
			continue
		}
		o := 0
		if f[8] == "O" {
			o = 1
		}
		if len(f) < 17+o {
			continue
		}
		num := func(s string) int { n, _ := strconv.Atoi(s); return n }
		ws = append(ws, window{id: f[1], title: strings.Join(f[16+o:], " "),
			w: num(f[4]), h: num(f[5]), x: num(f[6]), y: num(f[7]),
			shown: f[12+o] == "V" && f[11+o] == "0" && f[10+o] != "2"})
	}
	return ws
}

// overlays draws the windows the mirror misses onto the mirrored frame
// (ARGB8888 little-endian, i.e. B, G, R, A bytes).
func overlays(pix []byte, width, height, stride int) {
	var missing []window
	for _, w := range windows() {
		for _, m := range mirrorMisses {
			if w.shown && strings.Contains(w.title, m) {
				missing = append(missing, w)
			}
		}
	}
	if len(missing) == 0 {
		return
	}
	dir, err := os.MkdirTemp("/tmp", "tvremote-dump-")
	if err != nil {
		return
	}
	defer os.RemoveAll(dir)
	if exec.Command("enlightenment_info", "-dump_topvwins", dir).Run() != nil {
		return
	}
	for i := len(missing) - 1; i >= 0; i-- { // bottom first
		w := missing[i]
		files, _ := filepath.Glob(filepath.Join(dir, "*", w.id+".png"))
		if len(files) == 0 {
			continue
		}
		f, err := os.Open(files[0])
		if err != nil {
			continue
		}
		src, err := png.Decode(f)
		f.Close()
		if err != nil {
			continue
		}
		rgba := image.NewNRGBA(src.Bounds())
		draw.Draw(rgba, rgba.Bounds(), src, src.Bounds().Min, draw.Src)
		b := rgba.Bounds()
		for y := 0; y < b.Dy() && w.y+y < height; y++ {
			if w.y+y < 0 {
				continue
			}
			for x := 0; x < b.Dx() && w.x+x < width; x++ {
				if w.x+x < 0 {
					continue
				}
				s := rgba.Pix[y*rgba.Stride+x*4:]
				a := int(s[3])
				if a == 0 {
					continue
				}
				d := pix[(w.y+y)*stride+(w.x+x)*4:]
				d[0] = byte((int(s[2])*a + int(d[0])*(255-a)) / 255) // B
				d[1] = byte((int(s[1])*a + int(d[1])*(255-a)) / 255) // G
				d[2] = byte((int(s[0])*a + int(d[2])*(255-a)) / 255) // R
				if a > int(d[3]) {
					d[3] = byte(a)
				}
			}
		}
	}
}
