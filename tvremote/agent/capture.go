package main

import (
	"fmt"
	"image"
	"image/draw"
	"image/png"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// The screen is rebuilt from the compositor's window dump
// (enlightenment_info -dump_topvwins): every top-level window's buffer as a
// PNG, stacked here by position and order. The compositor's own capture
// interfaces (tizen_screenmirror, screenshooter) leak a full-screen graphics
// buffer (8 MB) on every frame on this TV until it runs out of memory and
// reboots; the dump frees everything. The dump blocks the compositor while it
// runs (about 1.5 s), so frames are taken at most every minGap after the last.
const (
	screenW, screenH = 1920, 1080
	minGap           = 2 * time.Second
)

var lastCapture time.Time

type window struct {
	id, title  string
	x, y, w, h int
	shown      bool // mapped, not iconified, not fully obscured
}

// windows lists the top-level windows, topmost first (enlightenment_info -topvwins).
func windows() ([]window, error) {
	out, err := exec.Command("enlightenment_info", "-topvwins").Output()
	if err != nil {
		return nil, fmt.Errorf("enlightenment_info -topvwins: %v", err)
	}
	var ws []window
	for _, l := range strings.Split(string(out), "\n") {
		f := strings.Fields(l)
		// No Win_ID RcsID PID w h x y [Foc] Dep Opaq Visi Icon Map Frame PL@ZPos Parent Title...
		// (Foc is "O" for the focused window and empty otherwise.)
		if len(f) < 16 || !strings.HasPrefix(f[1], "0x") {
			continue
		}
		o := 0
		if f[8] == "O" {
			o = 1
		}
		if len(f) < 16+o {
			continue
		}
		num := func(s string) int { n, _ := strconv.Atoi(s); return n }
		ws = append(ws, window{id: f[1], title: strings.Join(f[16+o:], " "),
			w: num(f[4]), h: num(f[5]), x: num(f[6]), y: num(f[7]),
			shown: f[12+o] == "V" && f[11+o] == "0" && f[10+o] != "2"})
	}
	return ws, nil
}

// capture returns the screen as a JPEG, scaled down n times.
func capture(n int) ([]byte, error) {
	if wait := minGap - time.Since(lastCapture); wait > 0 {
		time.Sleep(wait)
	}
	defer func() { lastCapture = time.Now() }()
	ws, err := windows()
	if err != nil {
		return nil, err
	}
	dir, err := os.MkdirTemp("/tmp", "tvremote-dump-")
	if err != nil {
		return nil, err
	}
	defer os.RemoveAll(dir)
	if out, err := exec.Command("enlightenment_info", "-dump_topvwins", dir).CombinedOutput(); err != nil {
		return nil, fmt.Errorf("window dump: %v: %s", err, strings.TrimSpace(string(out)))
	}
	screen := image.NewRGBA(image.Rect(0, 0, screenW, screenH))
	draw.Draw(screen, screen.Bounds(), image.Black, image.Point{}, draw.Src)
	for i := len(ws) - 1; i >= 0; i-- { // bottom first
		w := ws[i]
		if !w.shown || w.w < 2 || w.h < 2 {
			continue
		}
		img := load(dir, w.id)
		if img == nil {
			continue
		}
		if b := img.Bounds(); b.Dx() != w.w || b.Dy() != w.h {
			img = resize(img, w.w, w.h)
		}
		r := image.Rect(w.x, w.y, w.x+w.w, w.y+w.h)
		draw.Draw(screen, r, img, img.Bounds().Min, draw.Over)
	}
	return encode(downscale(screen, n))
}

// load decodes one window's buffer from the dump directory.
func load(dir, id string) image.Image {
	files, _ := filepath.Glob(filepath.Join(dir, "*", id+".png"))
	if len(files) == 0 {
		return nil
	}
	f, err := os.Open(files[0])
	if err != nil {
		return nil
	}
	defer f.Close()
	img, err := png.Decode(f)
	if err != nil {
		return nil
	}
	return img
}

// resize scales a window's buffer to its size on screen (nearest pixel).
func resize(src image.Image, w, h int) image.Image {
	b := src.Bounds()
	dst := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		sy := b.Min.Y + y*b.Dy()/h
		for x := 0; x < w; x++ {
			dst.Set(x, y, src.At(b.Min.X+x*b.Dx()/w, sy))
		}
	}
	return dst
}

// downscale makes the image n times smaller, averaging n x n blocks.
func downscale(src *image.RGBA, n int) *image.RGBA {
	if n <= 1 {
		return src
	}
	ow, oh := src.Bounds().Dx()/n, src.Bounds().Dy()/n
	dst := image.NewRGBA(image.Rect(0, 0, ow, oh))
	d := n * n
	for y := 0; y < oh; y++ {
		for x := 0; x < ow; x++ {
			var r, g, b int
			for dy := 0; dy < n; dy++ {
				p := src.Pix[(y*n+dy)*src.Stride+x*n*4:]
				for dx := 0; dx < n; dx++ {
					r += int(p[dx*4])
					g += int(p[dx*4+1])
					b += int(p[dx*4+2])
				}
			}
			o := dst.PixOffset(x, y)
			dst.Pix[o], dst.Pix[o+1], dst.Pix[o+2], dst.Pix[o+3] = byte(r/d), byte(g/d), byte(b/d), 255
		}
	}
	return dst
}
