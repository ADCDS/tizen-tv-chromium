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
	"io/fs"
	"log"
	"net/http"
	"os"
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
	cur  string // current app id
}

func (d *daemon) config() Config { return loadConfig(d.dir) }

// open shows the kiosk with the app's tab, starting the kiosk if needed.
func (d *daemon) open(a App) error {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.cur = a.ID
	if !chromiumRunning() {
		log.Printf("open %s: starting the kiosk", a.ID)
		d.tabs.reset()
		if err := startKiosk(d.dir, a.URL); err != nil {
			return err
		}
		if !d.win.waitVisible(150 * time.Second) {
			log.Printf("open %s: window not visible yet", a.ID)
		}
		return d.tabs.focus(a, 60*time.Second)
	}
	log.Printf("open %s", a.ID)
	d.win.send("show")
	err := d.tabs.focus(a, 15*time.Second)
	d.win.waitVisible(10 * time.Second)
	return err
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
		if err := d.open(a); err != nil {
			log.Printf("open %s: %v", a.ID, err)
			http.Error(w, err.Error(), http.StatusServiceUnavailable)
			return
		}
		reply(w, map[string]any{"ok": true, "app": a.ID})
	})
	mux.HandleFunc("/leave", func(w http.ResponseWriter, r *http.Request) {
		d.leave()
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
