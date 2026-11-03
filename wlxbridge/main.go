// wlxbridge shows an Xvfb screen as a wl_shell window on the TV's Wayland
// compositor (Tizen 3.0 E20, which only offers wl_shell/xdg_shell v1, so modern
// Chromium can't open a window there itself) and feeds the remote's keys into
// X through XTEST.
//
// Frames: Xvfb -fbdir exposes the screen as an XWD file; we mmap it and, when
// XDamage reports a change, copy it into a wl_shm buffer. The copy also sets
// every pixel's alpha byte: the TV blends the graphics plane over the video
// plane by alpha, and Xvfb leaves that byte at 0 (fully transparent).
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"

	"github.com/jezek/xgb"
	"github.com/jezek/xgb/damage"
	"github.com/jezek/xgb/xfixes"
	"github.com/jezek/xgb/xproto"
	"github.com/jezek/xgb/xtest"
)

var le = binary.LittleEndian

// ---- Wayland wire protocol (just what we need) ----

type wlConn struct {
	c   *net.UnixConn
	buf []byte
	fds []int
}

type wlEvent struct {
	obj  uint32
	op   uint16
	body []byte
	fd   int // -1 unless the event carried one (wl_keyboard.keymap)
}

func pad4(n int) int { return (n + 3) &^ 3 }

func (w *wlConn) send(obj uint32, op uint16, fd int, args ...interface{}) {
	b := make([]byte, 8, 64)
	for _, a := range args {
		switch v := a.(type) {
		case uint32:
			b = le.AppendUint32(b, v)
		case int32:
			b = le.AppendUint32(b, uint32(v))
		case string:
			n := len(v) + 1
			b = le.AppendUint32(b, uint32(n))
			s := make([]byte, pad4(n))
			copy(s, v)
			b = append(b, s...)
		default:
			panic(fmt.Sprintf("wl arg type %T", a))
		}
	}
	le.PutUint32(b[0:], obj)
	le.PutUint32(b[4:], uint32(len(b))<<16|uint32(op))
	var oob []byte
	if fd >= 0 {
		oob = syscall.UnixRights(fd)
	}
	if _, _, err := w.c.WriteMsgUnix(b, oob, nil); err != nil {
		log.Fatalf("wayland write: %v", err)
	}
}

func (w *wlConn) read(out chan<- wlEvent) {
	b, oob := make([]byte, 65536), make([]byte, 512)
	for {
		for len(w.buf) >= 8 {
			size := int(le.Uint32(w.buf[4:]) >> 16)
			if size < 8 || len(w.buf) < size {
				break
			}
			ev := wlEvent{obj: le.Uint32(w.buf), op: uint16(le.Uint32(w.buf[4:])), fd: -1,
				body: append([]byte(nil), w.buf[8:size]...)}
			w.buf = w.buf[size:]
			// Only wl_keyboard.keymap carries an fd in what we bind.
			if ev.op == 0 && len(ev.body) == 8 && len(w.fds) > 0 { // wl_keyboard.keymap: the only 8-byte op-0 event we get
				ev.fd, w.fds = w.fds[0], w.fds[1:]
			}
			out <- ev
		}
		n, oobn, _, _, err := w.c.ReadMsgUnix(b, oob)
		if err != nil {
			log.Fatalf("wayland read: %v", err)
		}
		if oobn > 0 {
			if msgs, err := syscall.ParseSocketControlMessage(oob[:oobn]); err == nil {
				for _, m := range msgs {
					if fds, err := syscall.ParseUnixRights(&m); err == nil {
						w.fds = append(w.fds, fds...)
					}
				}
			}
		}
		w.buf = append(w.buf, b[:n]...)
	}
}

func wlString(b []byte) (string, []byte) {
	n := int(le.Uint32(b))
	if n == 0 {
		return "", b[4:]
	}
	return string(b[4 : 4+n-1]), b[4+pad4(n):]
}

const idDisplay, idRegistry, idSync = 1, 2, 3

// Client object ids must be allocated in order with no gaps (libwayland
// rejects a skipped id as "invalid arguments"), so they are handed out in the
// order the objects are created. Optional objects get an id only if created.
var (
	nextID                                                      = uint32(4)
	idCompositor, idShm, idShell, idSeat, idKeyrouter, idScaler uint32
	idSurface, idPool, idShellSurf                              uint32
	idKeyboard, idViewport                                      uint32
)

// freeIDs holds ids the server released (wl_display.delete_id), e.g. frame
// callbacks; reusing them keeps the id space from growing every frame.
var freeIDs []uint32

func alloc() uint32 {
	if n := len(freeIDs); n > 0 {
		id := freeIDs[n-1]
		freeIDs = freeIDs[:n-1]
		return id
	}
	id := nextID
	nextID++
	return id
}

// ---- XWD framebuffer written by Xvfb -fbdir ----

type xwd struct {
	pix          []byte // mmap of the pixel area
	w, h, stride int
	bitsPerPixel int
}

func openXWD(path string) (*xwd, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	hdr := make([]byte, 100)
	if _, err := f.ReadAt(hdr, 0); err != nil {
		return nil, err
	}
	// Xvfb writes the header MSB-first; fall back to LSB if that looks wrong.
	var bo binary.ByteOrder = binary.BigEndian
	if bo.Uint32(hdr[0:]) > 4096 {
		bo = binary.LittleEndian
	}
	u := func(off int) int { return int(bo.Uint32(hdr[off:])) }
	x := &xwd{w: u(16), h: u(20), bitsPerPixel: u(44), stride: u(48)}
	if x.bitsPerPixel != 32 {
		return nil, fmt.Errorf("need a 32bpp framebuffer, got %d bpp (run Xvfb with depth 24)", x.bitsPerPixel)
	}
	off := u(0) + u(76)*12 // header_size + ncolors * sizeof(XWDColor)
	fi, err := f.Stat()
	if err != nil {
		return nil, err
	}
	m, err := syscall.Mmap(int(f.Fd()), 0, int(fi.Size()), syscall.PROT_READ, syscall.MAP_SHARED)
	if err != nil {
		return nil, err
	}
	if off+x.stride*x.h > len(m) {
		return nil, fmt.Errorf("xwd: pixels past end of file (off %d, %dx%d stride %d, size %d)", off, x.w, x.h, x.stride, len(m))
	}
	x.pix = m[off : off+x.stride*x.h]
	return x, nil
}

// copyOpaque copies one frame and forces alpha to 0xff (8 bytes at a time).
func copyOpaque(dst []byte, dstStride int, src *xwd) {
	row := src.w * 4
	for y := 0; y < src.h; y++ {
		s := src.pix[y*src.stride : y*src.stride+row]
		d := dst[y*dstStride : y*dstStride+row]
		s64 := unsafe.Slice((*uint64)(unsafe.Pointer(&s[0])), row/8)
		d64 := unsafe.Slice((*uint64)(unsafe.Pointer(&d[0])), row/8)
		for i := range s64 {
			d64[i] = s64[i] | 0xff000000ff000000
		}
	}
}

// ---- audio relay FIFO ----

// holdAudioFifo creates the FIFO and keeps a read-write fd on it for the life
// of the bridge, so the reader (pacat) never sees EOF when Chromium closes its
// audio device between videos, and the writer never blocks on open. It also
// shrinks the pipe from 64 KiB (~340 ms of 48 kHz s16 stereo) to 8 KiB
// (~42 ms): Chromium can't see latency past ALSA, so every buffered byte here
// is audio lagging behind the picture.
func holdAudioFifo(path string) {
	if err := syscall.Mkfifo(path, 0666); err != nil && err != syscall.EEXIST {
		log.Fatalf("audio fifo: %v", err)
	}
	fd, err := syscall.Open(path, syscall.O_RDWR, 0)
	if err != nil {
		log.Fatalf("audio fifo open: %v", err)
	}
	const fSetPipeSz = 1031 // F_SETPIPE_SZ
	if _, _, e := syscall.Syscall(syscall.SYS_FCNTL, uintptr(fd), fSetPipeSz, 8192); e != 0 {
		log.Printf("audio fifo: F_SETPIPE_SZ: %v (keeping default size)", e)
	}
	log.Printf("audio fifo %s held open", path)
	// fd intentionally never closed.
}

// ---- main ----

func parseCodes(s string) []uint32 {
	var out []uint32
	for _, f := range strings.Split(s, ",") {
		if n, err := strconv.Atoi(strings.TrimSpace(f)); err == nil {
			out = append(out, uint32(n))
		}
	}
	return out
}

func main() {
	display := flag.String("display", ":1", "X display (Xvfb)")
	fbPath := flag.String("fb", "/tmp/Xvfb_screen0", "Xvfb -fbdir screen file")
	wlPath := flag.String("wayland", "/run/wayland-0", "Wayland socket")
	outW := flag.Int("out-w", 1920, "on-screen width")
	outH := flag.Int("out-h", 1080, "on-screen height")
	fps := flag.Int("fps", 60, "max frames per second")
	nbuf := flag.Int("buffers", 3, "shm buffers (the TV compositor holds each ~50 ms)")
	grab := flag.String("grab", "9,10,11,12,13,14,15,16,17,18,19,36,67,68,69,70,95,96,111,113,114,116,174,176,182,208,209,216,252",
		"X keycodes (TV keymap) to grab from the TV via tizen_keyrouter")
	exitKey := flag.Uint("exit-key", 182, "keycode that quits (XF86Exit)")
	verbose := flag.Bool("v", false, "log every key")
	grabMode := flag.Uint("grab-mode", 2, "tizen_keyrouter grab mode: 2 topmost (our window on top), 3 overridable exclusive (another window, e.g. the mgl GPU window, is on top)")
	noFrames := flag.Bool("no-frames", false, "show a 1x1 surface instead of the Xvfb screen (GPU mode: Chromium's frames go to the mgl window; this one only holds key grabs)")
	anyFocus := flag.Bool("any-focus", false, "forward keys even without keyboard focus (when another window of ours, the GPU window, holds it)")
	audioFifo := flag.String("audio-fifo", "", "create and hold open this FIFO for the audio relay (Chromium/ALSA writes it, the TV's native pacat reads it)")
	flag.Parse()
	log.SetFlags(log.Ltime | log.Lmicroseconds)

	// TV keymap -> Xvfb (evdev rules) keycode, where they differ.
	remap := map[uint32]uint32{
		95:  117, // XF86LowerChannel -> Next (PageDown)
		96:  112, // XF86RaiseChannel -> Prior (PageUp)
		176: 113, // XF86AudioRewind -> Left (player seeks on arrows)
		216: 114, // XF86AudioNext (FF) -> Right
		252: 172, // XF86PlayBack -> XF86AudioPlay (MediaPlayPause)
	}
	repeatable := map[uint32]bool{111: true, 113: true, 114: true, 116: true, 95: true, 96: true}

	if *audioFifo != "" {
		holdAudioFifo(*audioFifo)
	}

	var fb *xwd
	if *noFrames {
		fb = &xwd{w: 1, h: 1, stride: 4, bitsPerPixel: 32, pix: make([]byte, 4)}
		*outW, *outH = 1, 1
	} else {
		var err error
		if fb, err = openXWD(*fbPath); err != nil {
			log.Fatalf("framebuffer: %v", err)
		}
	}
	log.Printf("xvfb framebuffer %dx%d stride %d", fb.w, fb.h, fb.stride)

	// X: XDamage for change detection, XTEST for keys.
	xc, err := xgb.NewConnDisplay(*display)
	if err != nil {
		log.Fatalf("X connect %s: %v", *display, err)
	}
	for _, init := range []func(*xgb.Conn) error{xfixes.Init, damage.Init, xtest.Init} {
		if err := init(xc); err != nil {
			log.Fatalf("X extension: %v", err)
		}
	}
	if _, err := xfixes.QueryVersion(xc, 5, 0).Reply(); err != nil {
		log.Fatalf("xfixes: %v", err)
	}
	if _, err := damage.QueryVersion(xc, 1, 1).Reply(); err != nil {
		log.Fatalf("damage: %v", err)
	}
	root := xproto.Setup(xc).DefaultScreen(xc).Root
	dmg, _ := damage.NewDamageId(xc)
	if err := damage.CreateChecked(xc, dmg, xproto.Drawable(root), damage.ReportLevelNonEmpty).Check(); err != nil {
		log.Fatalf("damage create: %v", err)
	}
	// Park the pointer in the bottom-right corner so the cursor stays out of view.
	scr := xproto.Setup(xc).DefaultScreen(xc)
	xtest.FakeInput(xc, xproto.MotionNotify, 0, 0, root, int16(scr.WidthInPixels-1), int16(scr.HeightInPixels-1), 0)
	dirty := make(chan struct{}, 1)
	go func() {
		for {
			ev, err := xc.WaitForEvent()
			if ev == nil && err == nil {
				log.Fatal("X connection closed")
			}
			if _, ok := ev.(damage.NotifyEvent); ok {
				select {
				case dirty <- struct{}{}:
				default:
				}
			}
		}
	}()
	dirty <- struct{}{} // first frame

	// Wayland.
	uc, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: *wlPath, Net: "unix"})
	if err != nil {
		log.Fatalf("wayland connect: %v", err)
	}
	w := &wlConn{c: uc}
	events := make(chan wlEvent, 256)
	go w.read(events)

	w.send(idDisplay, 1, -1, uint32(idRegistry))
	w.send(idDisplay, 0, -1, uint32(idSync))
	globals := map[string][2]uint32{}
	for done := false; !done; {
		ev := <-events
		switch {
		case ev.obj == idRegistry && ev.op == 0:
			iface, rest := wlString(ev.body[4:])
			globals[iface] = [2]uint32{le.Uint32(ev.body), le.Uint32(rest)}
		case ev.obj == idSync:
			done = true
		}
	}
	bind := func(iface string, maxv uint32) uint32 { // 0 if the global is missing
		g, ok := globals[iface]
		if !ok {
			return 0
		}
		id := alloc()
		w.send(idRegistry, 0, -1, g[0], iface, min(g[1], maxv), id)
		return id
	}
	for _, g := range []struct {
		n  string
		id *uint32
		v  uint32
	}{{"wl_compositor", &idCompositor, 3}, {"wl_shm", &idShm, 1}, {"wl_shell", &idShell, 1}, {"wl_seat", &idSeat, 4}} {
		if *g.id = bind(g.n, g.v); *g.id == 0 {
			log.Fatalf("compositor lacks %s", g.n)
		}
	}
	idKeyrouter = bind("tizen_keyrouter", 1)
	haveKeyrouter := idKeyrouter != 0
	scale := *outW != fb.w || *outH != fb.h
	if scale {
		if idScaler = bind("wl_scaler", 2); idScaler == 0 {
			log.Printf("no wl_scaler: showing %dx%d unscaled", fb.w, fb.h)
			scale = false
		}
	}

	// N shm buffers, same layout as the framebuffer.
	stride := fb.w * 4
	frame := stride * fb.h
	n := *nbuf
	pf, err := os.CreateTemp("/tmp", "wlxbridge-")
	if err != nil {
		log.Fatal(err)
	}
	os.Remove(pf.Name())
	if err := pf.Truncate(int64(n * frame)); err != nil {
		log.Fatal(err)
	}
	poolMem, err := syscall.Mmap(int(pf.Fd()), 0, n*frame, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		log.Fatal(err)
	}
	idSurface = alloc()
	w.send(idCompositor, 0, -1, idSurface)
	idPool = alloc()
	w.send(idShm, 0, int(pf.Fd()), idPool, int32(n*frame))
	bufIDs := make([]uint32, n)
	bufOff := map[uint32]int{}
	for i := range bufIDs {
		bufIDs[i] = alloc()
		bufOff[bufIDs[i]] = i * frame
		w.send(idPool, 0, -1, bufIDs[i], int32(i*frame), int32(fb.w), int32(fb.h), int32(stride), uint32(1)) // XRGB8888
	}
	idShellSurf = alloc()
	w.send(idShell, 0, -1, idShellSurf, idSurface)
	w.send(idShellSurf, 3, -1)           // set_toplevel
	w.send(idShellSurf, 8, -1, "Chromium") // set_title
	if scale {
		idViewport = alloc()
		w.send(idScaler, 1, -1, idViewport, idSurface)        // get_viewport
		w.send(idViewport, 3, -1, int32(*outW), int32(*outH)) // set_destination
	}
	idKeyboard = alloc()
	w.send(idSeat, 1, -1, idKeyboard) // get_keyboard
	if haveKeyrouter {
		for _, k := range parseCodes(*grab) {
			w.send(idKeyrouter, 0, -1, uint32(idSurface), k, uint32(*grabMode)) // set_keygrab
		}
	} else {
		log.Print("no tizen_keyrouter: remote keys will mostly go to the TV")
	}

	busy := map[uint32]bool{}
	freeBuf := func() uint32 {
		for _, id := range bufIDs {
			if !busy[id] {
				return id
			}
		}
		return 0
	}
	// Pace on the compositor: after each commit, wait for its frame callback
	// (it's ready for the next frame) before committing again; fall back to a
	// timeout in case the surface is hidden and callbacks stop.
	var frameCB uint32
	var frameSent time.Time
	focused := false
	repeatDelay, repeatRate := 400*time.Millisecond, 50*time.Millisecond
	var repeatKey uint32
	repeatTimer := time.NewTimer(time.Hour)
	repeatTimer.Stop()
	ticker := time.NewTicker(time.Second / time.Duration(*fps))
	pending := false
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)

	fake := func(press bool, code uint32) {
		t := byte(xproto.KeyRelease)
		if press {
			t = xproto.KeyPress
		}
		xtest.FakeInput(xc, t, byte(code), 0, root, 0, 0, 0)
	}
	// Stats (logged every 10 s with -v): what limits the frame rate?
	frames, damages, blocked, cbWaits := 0, 0, 0, 0
	var copyTime, releaseWait time.Duration
	releases := 0
	attachedAt := map[uint32]time.Time{}
	lastStat := time.Now()
	log.Printf("up: %dx%d -> %dx%d, keyrouter=%v", fb.w, fb.h, *outW, *outH, haveKeyrouter)

	for {
		select {
		case <-sig:
			log.Print("signal, exiting")
			return
		case <-dirty:
			pending = true
			damages++
		case <-ticker.C:
			if !pending {
				break
			}
			if frameCB != 0 && time.Since(frameSent) < 100*time.Millisecond {
				cbWaits++
				break
			}
			buf := freeBuf()
			if buf == 0 {
				blocked++
				break
			}
			pending = false
			// Freeze X for the copy (Xvfb processes no other client's requests
			// while grabbed), so a frame is never half old, half new. The round
			// trip makes sure the grab is in effect before reading pixels.
			xproto.GrabServer(xc)
			xproto.GetInputFocus(xc).Reply()
			damage.Subtract(xc, dmg, xfixes.Region(xfixes.RegionNone), xfixes.Region(xfixes.RegionNone))
			t0 := time.Now()
			off := bufOff[buf]
			copyOpaque(poolMem[off:off+frame], stride, fb)
			copyTime += time.Since(t0)
			xproto.UngrabServer(xc)
			frameCB = alloc()
			frameSent = time.Now()
			w.send(idSurface, 3, -1, frameCB)                                      // frame callback
			w.send(idSurface, 1, -1, buf, int32(0), int32(0))                      // attach
			w.send(idSurface, 2, -1, int32(0), int32(0), int32(fb.w), int32(fb.h)) // damage
			w.send(idSurface, 6, -1)                                               // commit
			busy[buf] = true
			attachedAt[buf] = time.Now()
			frames++
			if *verbose && time.Since(lastStat) > 10*time.Second {
				avg := func(d time.Duration, n int) time.Duration {
					if n == 0 {
						return 0
					}
					return (d / time.Duration(n)).Round(100 * time.Microsecond)
				}
				log.Printf("%d frames in %s; x damage %d; no free buffer %d; waiting for compositor %d; copy avg %s; release avg %s",
					frames, time.Since(lastStat).Round(time.Second), damages, blocked, cbWaits,
					avg(copyTime, frames), avg(releaseWait, releases))
				frames, damages, blocked, cbWaits, releases, lastStat = 0, 0, 0, 0, 0, time.Now()
				copyTime, releaseWait = 0, 0
			}
		case <-repeatTimer.C:
			if repeatKey != 0 && (focused || *anyFocus) {
				fake(true, repeatKey)
				repeatTimer.Reset(repeatRate)
			}
		case ev := <-events:
			switch {
			case ev.obj == idDisplay && ev.op == 0:
				m, _ := wlString(ev.body[8:])
				log.Fatalf("wayland protocol error obj=%d code=%d: %s", le.Uint32(ev.body), le.Uint32(ev.body[4:]), m)
			case ev.obj == idShellSurf && ev.op == 0: // ping
				w.send(idShellSurf, 0, -1, le.Uint32(ev.body))
			case ev.obj == idDisplay && ev.op == 1: // delete_id: the server freed an id
				freeIDs = append(freeIDs, le.Uint32(ev.body))
			case ev.obj == frameCB && frameCB != 0 && ev.op == 0: // frame callback done
				frameCB = 0
			case busy[ev.obj] && ev.op == 0 && len(ev.body) == 0: // wl_buffer.release
				busy[ev.obj] = false
				if t, ok := attachedAt[ev.obj]; ok {
					releaseWait += time.Since(t)
					releases++
				}
			case ev.obj == idKeyboard && ev.op == 0: // keymap: not needed
				if ev.fd >= 0 {
					syscall.Close(ev.fd)
				}
			case ev.obj == idKeyboard && ev.op == 1:
				focused = true
				log.Print("keyboard focus in")
			case ev.obj == idKeyboard && ev.op == 2:
				focused = false
				repeatKey = 0
				repeatTimer.Stop()
				log.Print("keyboard focus out")
			case ev.obj == idKeyboard && ev.op == 3: // key(serial, time, key, state)
				code := le.Uint32(ev.body[8:]) + 8
				down := le.Uint32(ev.body[12:]) == 1
				if *verbose {
					log.Printf("key %d %v focused=%v", code, down, focused)
				}
				if code == uint32(*exitKey) && !down {
					log.Print("exit key, exiting")
					return
				}
				if !focused && !*anyFocus {
					break
				}
				x := code
				if m, ok := remap[code]; ok {
					x = m
				}
				fake(down, x)
				if down && repeatable[code] {
					repeatKey = x
					repeatTimer.Reset(repeatDelay)
				} else if !down && repeatKey == x {
					repeatKey = 0
					repeatTimer.Stop()
				}
			case ev.obj == idKeyboard && ev.op == 5: // repeat_info(rate, delay)
				if rate := int32(le.Uint32(ev.body)); rate > 0 {
					repeatRate = time.Second / time.Duration(rate)
				}
				if d := int32(le.Uint32(ev.body[4:])); d > 0 {
					repeatDelay = time.Duration(d) * time.Millisecond
				}
			case ev.obj == idKeyrouter && ev.op == 0: // keygrab_notify
				if e := le.Uint32(ev.body[12:]); e != 0 && *verbose {
					log.Printf("keygrab %d failed: error %d", le.Uint32(ev.body[4:]), e)
				}
			}
		}
	}
}
