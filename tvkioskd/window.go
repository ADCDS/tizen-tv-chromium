package main

import (
	"bufio"
	"log"
	"net"
	"strings"
	"sync"
	"time"
)

// window talks to mgl-server's window socket (GPU mode): it learns whether the
// kiosk's window is visible and sends it show/hide.
type window struct {
	path     string
	mu       sync.Mutex
	cond     *sync.Cond
	conn     net.Conn
	visible  bool
	onHidden func()
}

func newWindow(path string) *window {
	w := &window{path: path}
	w.cond = sync.NewCond(&w.mu)
	return w
}

func (w *window) run() {
	for {
		c, err := net.Dial("unix", w.path)
		if err != nil {
			time.Sleep(time.Second)
			continue
		}
		w.mu.Lock()
		w.conn = c
		w.mu.Unlock()
		log.Printf("window: connected to %s", w.path)
		sc := bufio.NewScanner(c)
		for sc.Scan() {
			f := strings.Fields(sc.Text())
			if len(f) == 2 && f[0] == "visible" {
				w.setVisible(f[1] == "1")
			}
		}
		c.Close()
		w.mu.Lock()
		w.conn = nil
		w.mu.Unlock()
		w.setVisible(false)
		log.Print("window: disconnected")
		time.Sleep(time.Second)
	}
}

func (w *window) setVisible(v bool) {
	w.mu.Lock()
	was := w.visible
	w.visible = v
	w.cond.Broadcast()
	w.mu.Unlock()
	if v != was {
		log.Printf("window: visible=%v", v)
		if !v && w.onHidden != nil {
			w.onHidden()
		}
	}
}

func (w *window) isVisible() bool {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.visible
}

func (w *window) send(cmd string) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.conn == nil {
		log.Printf("window: %s: not connected", cmd)
		return
	}
	w.conn.SetWriteDeadline(time.Now().Add(2 * time.Second))
	if _, err := w.conn.Write([]byte(cmd + "\n")); err != nil {
		log.Printf("window: %s: %v", cmd, err)
	}
}

// waitVisible waits until the window is on screen (or the timeout passes).
func (w *window) waitVisible(timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	t := time.AfterFunc(timeout, func() { w.mu.Lock(); w.cond.Broadcast(); w.mu.Unlock() })
	defer t.Stop()
	w.mu.Lock()
	defer w.mu.Unlock()
	for !(w.visible && w.conn != nil) && time.Now().Before(deadline) {
		w.cond.Wait()
	}
	return w.visible && w.conn != nil
}
