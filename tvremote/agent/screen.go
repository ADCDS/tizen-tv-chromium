package main

import (
	"errors"
	"fmt"
	"net"
	"os"
	"syscall"
	"time"
)

// ---- minimal Wayland client (wire protocol) ----

type event struct {
	obj  uint32
	op   uint16
	body []byte
}

type wl struct {
	c      *net.UnixConn
	events chan event
	errs   chan error
	next   uint32
}

func pad4(n int) int { return (n + 3) &^ 3 }

func (w *wl) id() uint32 { w.next++; return w.next }

func (w *wl) send(obj uint32, op uint16, fd int, args ...any) error {
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
			panic(fmt.Sprintf("wl arg %T", a))
		}
	}
	le.PutUint32(b[0:], obj)
	le.PutUint32(b[4:], uint32(len(b))<<16|uint32(op))
	var oob []byte
	if fd >= 0 {
		oob = syscall.UnixRights(fd)
	}
	_, _, err := w.c.WriteMsgUnix(b, oob, nil)
	return err
}

func (w *wl) read() {
	var buf []byte
	b := make([]byte, 65536)
	for {
		n, err := w.c.Read(b)
		if err != nil {
			w.errs <- err
			return
		}
		buf = append(buf, b[:n]...)
		for len(buf) >= 8 {
			size := int(le.Uint32(buf[4:]) >> 16)
			if size < 8 || len(buf) < size {
				break
			}
			w.events <- event{le.Uint32(buf), uint16(le.Uint32(buf[4:])), append([]byte(nil), buf[8:size]...)}
			buf = buf[size:]
		}
	}
}

// wait returns the next event for which accept is true, failing on protocol
// errors, a closed connection or the timeout.
func (w *wl) wait(timeout time.Duration, accept func(event) bool) (event, error) {
	t := time.NewTimer(timeout)
	defer t.Stop()
	for {
		select {
		case ev := <-w.events:
			if ev.obj == 1 && ev.op == 0 { // wl_display.error(object, code, message)
				return ev, fmt.Errorf("wayland error %d on object %d", le.Uint32(ev.body[4:]), le.Uint32(ev.body))
			}
			if accept(ev) {
				return ev, nil
			}
		case err := <-w.errs:
			return event{}, err
		case <-t.C:
			return event{}, errors.New("timeout")
		}
	}
}

// roundtrip delivers every pending event to f, then returns.
func (w *wl) roundtrip(f func(event)) error {
	cb := w.id()
	if err := w.send(1, 0, -1, cb); err != nil { // wl_display.sync
		return err
	}
	_, err := w.wait(3*time.Second, func(ev event) bool {
		if ev.obj == cb {
			return true
		}
		f(ev)
		return false
	})
	return err
}

func wlString(b []byte) (string, []byte) {
	n := int(le.Uint32(b))
	if n == 0 {
		return "", b[4:]
	}
	return string(b[4 : 4+n-1]), b[4+pad4(n):]
}

// ---- screen capture through tizen_screenmirror ----

type screen struct {
	w          *wl
	pix        []byte
	file       *os.File
	width, hgt int
	stride     int
	buffer     uint32
	mirror     uint32
	started    bool
}

func openScreen() (*screen, error) {
	c, err := net.DialUnix("unix", nil, &net.UnixAddr{Name: "/run/wayland-0", Net: "unix"})
	if err != nil {
		return nil, err
	}
	w := &wl{c: c, events: make(chan event, 256), errs: make(chan error, 1), next: 1}
	go w.read()
	s := &screen{w: w, width: 1920, hgt: 1080}
	fail := func(err error) (*screen, error) { c.Close(); return nil, err }

	registry := w.id()
	w.send(1, 1, -1, registry) // wl_display.get_registry
	globals := map[string][2]uint32{}
	if err := w.roundtrip(func(ev event) {
		if ev.obj == registry && ev.op == 0 { // global(name, interface, version)
			name := le.Uint32(ev.body)
			iface, rest := wlString(ev.body[4:])
			globals[iface] = [2]uint32{name, le.Uint32(rest)}
		}
	}); err != nil {
		return fail(err)
	}
	bind := func(iface string, version uint32) (uint32, error) {
		g, ok := globals[iface]
		if !ok {
			return 0, fmt.Errorf("compositor has no %s", iface)
		}
		id := w.id()
		return id, w.send(registry, 0, -1, g[0], iface, min(version, g[1]), id)
	}
	shm, err := bind("wl_shm", 1)
	if err != nil {
		return fail(err)
	}
	output, err := bind("wl_output", 1)
	if err != nil {
		return fail(err)
	}
	shooter, err := bind("tizen_screenshooter", 1)
	if err != nil {
		return fail(err)
	}
	formats := map[uint32]bool{}
	if err := w.roundtrip(func(ev event) {
		switch {
		case ev.obj == output && ev.op == 1 && le.Uint32(ev.body)&1 != 0: // mode(flags, w, h, refresh), current
			s.width, s.hgt = int(int32(le.Uint32(ev.body[4:]))), int(int32(le.Uint32(ev.body[8:])))
		case ev.obj == shooter && ev.op == 0: // format(f)
			formats[le.Uint32(ev.body)] = true
		case ev.obj == shooter && ev.op == 1: // screenshooter_notify(state)
			fmt.Fprintf(os.Stderr, "screenshooter notify %d\n", le.Uint32(ev.body))
		}
	}); err != nil {
		return fail(err)
	}
	// wl_shm buffers take wl_shm format codes: 0 = ARGB8888 (always supported).
	_ = formats
	format := uint32(0)

	s.stride = s.width * 4
	size := s.stride * s.hgt
	s.file, err = os.CreateTemp("/tmp", "tvremote-")
	if err != nil {
		return fail(err)
	}
	os.Remove(s.file.Name())
	if err := s.file.Truncate(int64(size)); err != nil {
		return fail(err)
	}
	if s.pix, err = syscall.Mmap(int(s.file.Fd()), 0, size, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED); err != nil {
		return fail(err)
	}
	pool := w.id()
	w.send(shm, 0, int(s.file.Fd()), pool, int32(size)) // create_pool
	s.buffer = w.id()
	w.send(pool, 0, -1, s.buffer, int32(0), int32(s.width), int32(s.hgt), int32(s.stride), format) // create_buffer
	s.mirror = w.id()
	w.send(shooter, 0, -1, s.mirror, output) // get_screenmirror
	if err := w.roundtrip(func(event) {}); err != nil {
		return fail(err)
	}
	return s, nil
}

func (s *screen) capture(scale int) ([]byte, error) {
	w := s.w
	// One shot per frame: a mirror left running stops refreshing its buffer.
	if err := w.send(s.mirror, 2, -1, s.buffer); err != nil { // queue(buffer)
		return nil, err
	}
	if err := w.send(s.mirror, 4, -1); err != nil { // start
		return nil, err
	}
	_, err := w.wait(5*time.Second, func(ev event) bool {
		return ev.obj == s.mirror && (ev.op == 0 || ev.op == 2) // dequeued(buffer) or stop
	})
	w.send(s.mirror, 5, -1) // stop
	if err != nil {
		return nil, fmt.Errorf("screen mirror: %v", err)
	}
	overlays(s.pix, s.width, s.hgt, s.stride)
	return encode(downscale(s.pix, s.width, s.hgt, s.stride, scale))
}

func (s *screen) close() {
	s.w.c.Close()
	if s.pix != nil {
		syscall.Munmap(s.pix)
	}
	if s.file != nil {
		s.file.Close()
	}
}
