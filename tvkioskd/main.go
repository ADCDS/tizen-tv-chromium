// tvkioskd: the kiosk's helper on the TV. It runs as root outside the chroot,
// starts and stops the kiosk, shows and hides its window, and keeps one
// Chromium tab per app, so that the TV's own app tiles can open an app:
//
//	GET  /open?app=ID  show the kiosk with that app's tab (starting the kiosk
//	                   if needed); answers once the window is on screen
//	POST /leave        pause media, drop throwaway tabs, hide the kiosk
//	GET  /state        JSON: current app, running, visible, apps
//	GET  /start        the browser's start page
//
// Opening the TV's own web browser opens the kiosk's browser instead (-takeover).
//
// It listens on 127.0.0.1 only. Settings, written by `kiosk install` into the
// kiosk directory on the stick: kiosk.env (KIOSK_* for kiosk-tv.sh) and
// apps.json. kiosk-tv.sh from that directory is fed to bash on stdin.
package main

import (
	"embed"
	"encoding/json"
	"flag"
	"fmt"
	"io/fs"
	"log"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

//go:embed start
var startFiles embed.FS

const listenAddr = "127.0.0.1:7770"

type App struct {
	ID   string `json:"id"`
	Name string `json:"name"`
	URL  string `json:"url"`
	Keep bool   `json:"keep"` // keep the tab (and its state) when leaving
}

type Config struct {
	Apps   []App  `json:"apps"`
	Search string `json:"search"`
	Lang   string `json:"lang"`
}

// loadConfig reads apps.json; the browser (start page) is always an app.
func loadConfig(dir string) Config {
	var c Config
	if b, err := os.ReadFile(filepath.Join(dir, "apps.json")); err == nil {
		if err := json.Unmarshal(b, &c); err != nil {
			log.Printf("apps.json: %v", err)
		}
	}
	if c.Search == "" {
		c.Search = "https://www.google.com/search?q=%s"
	}
	if _, ok := c.app("browser"); !ok {
		name := "Browser"
		if strings.HasPrefix(c.Lang, "pt") {
			name = "Navegador"
		}
		c.Apps = append(c.Apps, App{ID: "browser", Name: name, URL: "http://" + listenAddr + "/start", Keep: true})
	}
	return c
}

func (c Config) app(id string) (App, bool) {
	for _, a := range c.Apps {
		if a.ID == id {
			return a, true
		}
	}
	return App{}, false
}

type daemon struct {
	dir  string
	mu   sync.Mutex // serializes open/leave
	win  *window
	tabs *tabs
	cur  string    // current app id
	left time.Time // when the kiosk was last hidden by leave (zero: shown)
}

// minFree: below this much available memory (MB) a hidden kiosk is stopped.
// The TV has 1.3 GB; a loaded Chromium costs about 280 MB, and the TV's own
// app launcher (which also starts our tiles) fails when memory runs short.
var minFree = flag.Int("min-free", 250, "stop the hidden kiosk when the TV has less than this many MB available")

// memWatch stops the kiosk while it is hidden if the TV runs short of memory;
// the next open then starts it again (about 15 s instead of instantly).
func (d *daemon) memWatch() {
	for range time.Tick(3 * time.Second) {
		d.mu.Lock()
		if !d.left.IsZero() && time.Since(d.left) > 3*time.Second && chromiumRunning() {
			if avail := memAvailableMB(); avail > 0 && avail < *minFree {
				log.Printf("low memory (%d MB available) while hidden: stopping the kiosk", avail)
				d.tabs.reset()
				kiosk(d.dir, "stop")
			}
		}
		d.mu.Unlock()
	}
}

// pressKey presses a remote key (evdev code) on the remote's input device:
// struct input_event on the TV's 32-bit kernel (timeval, type, code, value).
func pressKey(code uint16) {
	f, err := os.OpenFile("/dev/input/event0", os.O_WRONLY, 0)
	if err != nil {
		log.Printf("key %d: %v", code, err)
		return
	}
	defer f.Close()
	ev := func(typ, code uint16, value uint32) []byte {
		b := make([]byte, 16)
		b[8], b[9] = byte(typ), byte(typ>>8)
		b[10], b[11] = byte(code), byte(code>>8)
		b[12], b[13], b[14], b[15] = byte(value), byte(value>>8), byte(value>>16), byte(value>>24)
		return b
	}
	syn := ev(0, 0, 0)
	f.Write(append(ev(1, code, 1), syn...))
	time.Sleep(80 * time.Millisecond)
	f.Write(append(ev(1, code, 0), syn...))
}

func memAvailableMB() int {
	b, err := os.ReadFile("/proc/meminfo")
	if err != nil {
		return -1
	}
	for _, l := range strings.Split(string(b), "\n") {
		var kb int
		if n, _ := fmt.Sscanf(l, "MemAvailable: %d kB", &kb); n == 1 {
			return kb / 1024
		}
	}
	return -1
}

func (d *daemon) config() Config { return loadConfig(d.dir) }

// open shows the kiosk with the app's tab, starting the kiosk if needed.
// from is the TV app that asked (a tile), closed once the app is up: a tile
// stays in front of our window until it is gone.
func (d *daemon) open(a App, from string) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.cur = a.ID
	d.left = time.Time{}
	if !chromiumRunning() {
		log.Printf("open %s: starting the kiosk", a.ID)
		d.tabs.reset()
		if err := startKiosk(d.dir, a.URL); err != nil {
			return err
		}
		if !d.win.waitVisible(150 * time.Second) {
			log.Printf("open %s: window not visible yet", a.ID)
		}
		err := d.tabs.focus(a, 60*time.Second)
		d.dismiss(from)
		return err
	}
	log.Printf("open %s", a.ID)
	d.win.send("show")
	err := d.tabs.focus(a, 15*time.Second)
	d.dismiss(from)
	return err
}

// dismiss closes the TV app that opened us (a tile), then raises our window
// again, which the tile was covering.
func (d *daemon) dismiss(from string) {
	go func() {
		if validAppID(from) {
			exec.Command("app_launcher", "-t", from).Run()
		}
		time.Sleep(500 * time.Millisecond)
		d.win.send("show")
	}()
}

func validAppID(s string) bool {
	if s == "" || len(s) > 64 {
		return false
	}
	for _, c := range s {
		if !(c == '.' || c >= '0' && c <= '9' || c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z') {
			return false
		}
	}
	return true
}

// leave pauses media, closes tabs of apps that aren't kept, and hides the kiosk.
func (d *daemon) leave() {
	d.mu.Lock()
	defer d.mu.Unlock()
	log.Print("leave")
	d.tabs.pauseAll()
	var drop []string
	for _, a := range d.config().Apps {
		if !a.Keep {
			drop = append(drop, a.ID)
		}
	}
	d.tabs.drop(drop)
	d.win.send("hide")
	d.left = time.Now()
}

func main() {
	dir := flag.String("dir", "", "kiosk directory on the stick (default: kiosk/ on the partition labelled tvchrome)")
	takeoverApp := flag.String("takeover", "org.tizen.browser", "TV app replaced by the kiosk's browser when opened (empty: none)")
	flag.Parse()
	log.SetFlags(log.Ltime | log.Lmicroseconds)
	if *dir == "" {
		*dir = waitForStore(10 * time.Minute)
	}
	log.Printf("tvkioskd: kiosk directory %s, listening on %s", *dir, listenAddr)

	d := &daemon{dir: *dir, win: newWindow(chroot + "/tmp/mglwin.sock"), tabs: newTabs()}
	d.win.onHidden = func() { go d.tabs.pauseAll() } // covered by the TV's own UI or another app
	go d.win.run()
	go d.memWatch()
	if *takeoverApp != "" {
		go d.takeover(*takeoverApp)
	}

	start, _ := fs.Sub(startFiles, "start")
	mux := http.NewServeMux()
	mux.Handle("/start/", http.StripPrefix("/start/", http.FileServer(http.FS(start))))
	mux.HandleFunc("/start", func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, "/start/", http.StatusFound) })
	mux.HandleFunc("/open", func(w http.ResponseWriter, r *http.Request) {
		a, ok := d.config().app(r.URL.Query().Get("app"))
		if !ok {
			http.Error(w, "unknown app", http.StatusNotFound)
			return
		}
		if err := d.open(a, r.URL.Query().Get("from")); err != nil {
			log.Printf("open %s: %v", a.ID, err)
			http.Error(w, err.Error(), http.StatusServiceUnavailable)
			return
		}
		reply(w, map[string]any{"ok": true, "app": a.ID})
	})
	mux.HandleFunc("/leave", func(w http.ResponseWriter, r *http.Request) {
		d.leave()
		if r.URL.Query().Get("then") == "home" { // Home in the kiosk: the TV's home bar
			go func() { time.Sleep(600 * time.Millisecond); pressKey(63) }()
		}
		reply(w, map[string]any{"ok": true})
	})
	mux.HandleFunc("/state", func(w http.ResponseWriter, r *http.Request) {
		c := d.config()
		reply(w, map[string]any{"app": d.cur, "running": chromiumRunning(), "visible": d.win.isVisible(),
			"apps": c.Apps, "search": c.Search, "lang": c.Lang})
	})
	log.Fatal(http.ListenAndServe(listenAddr, mux))
}

func reply(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*") // the tile apps and the extension
	json.NewEncoder(w).Encode(v)
}
