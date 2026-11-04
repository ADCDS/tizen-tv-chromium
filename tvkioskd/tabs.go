package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net/http"
	"net/url"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const devtools = "http://127.0.0.1:9222" // the kiosk's Chromium (kiosk-tv.sh)

// Pauses every <video>/<audio>, including in same-origin frames.
const pauseJS = `(function p(d){try{d.querySelectorAll('video,audio').forEach(function(m){try{m.pause()}catch(e){}});` +
	`d.querySelectorAll('iframe').forEach(function(f){try{p(f.contentDocument)}catch(e){}})}catch(e){}})(document)`

// cdp is a minimal DevTools protocol client on the browser endpoint.
type cdp struct {
	mu      sync.Mutex
	c       *websocket.Conn
	id      int
	pending map[int]chan cdpReply
	dead    bool
}

type cdpReply struct {
	Result json.RawMessage
	Error  *struct{ Message string }
}

func dialCDP() (*cdp, error) {
	cl := http.Client{Timeout: 3 * time.Second}
	r, err := cl.Get(devtools + "/json/version")
	if err != nil {
		return nil, err
	}
	defer r.Body.Close()
	var v struct{ WebSocketDebuggerUrl string }
	if err := json.NewDecoder(r.Body).Decode(&v); err != nil {
		return nil, err
	}
	c, _, err := websocket.DefaultDialer.Dial(v.WebSocketDebuggerUrl, nil)
	if err != nil {
		return nil, err
	}
	p := &cdp{c: c, pending: map[int]chan cdpReply{}}
	go p.read()
	return p, nil
}

func (p *cdp) read() {
	for {
		var m struct {
			ID     int
			Result json.RawMessage
			Error  *struct{ Message string }
		}
		if err := p.c.ReadJSON(&m); err != nil {
			p.mu.Lock()
			p.dead = true
			for _, ch := range p.pending {
				close(ch)
			}
			p.pending = map[int]chan cdpReply{}
			p.mu.Unlock()
			return
		}
		p.mu.Lock()
		ch := p.pending[m.ID]
		delete(p.pending, m.ID)
		p.mu.Unlock()
		if ch != nil {
			ch <- cdpReply{m.Result, m.Error}
		}
	}
}

// call sends a command (to a target session when sid != "") and waits for its result.
func (p *cdp) call(method string, params map[string]any, sid string, out any) error {
	p.mu.Lock()
	if p.dead {
		p.mu.Unlock()
		return errors.New("devtools connection closed")
	}
	p.id++
	id := p.id
	ch := make(chan cdpReply, 1)
	p.pending[id] = ch
	msg := map[string]any{"id": id, "method": method, "params": params}
	if sid != "" {
		msg["sessionId"] = sid
	}
	err := p.c.WriteJSON(msg)
	p.mu.Unlock()
	if err != nil {
		return err
	}
	select {
	case r, ok := <-ch:
		if !ok {
			return errors.New("devtools connection closed")
		}
		if r.Error != nil {
			return fmt.Errorf("%s: %s", method, r.Error.Message)
		}
		if out != nil {
			return json.Unmarshal(r.Result, out)
		}
		return nil
	case <-time.After(10 * time.Second):
		return fmt.Errorf("%s: timeout", method)
	}
}

type target struct {
	TargetID string `json:"targetId"`
	Type     string `json:"type"`
	URL      string `json:"url"`
}

// tabs keeps one Chromium tab per app.
type tabs struct {
	mu    sync.Mutex
	p     *cdp
	byApp map[string]string // app id -> target id
}

func newTabs() *tabs { return &tabs{byApp: map[string]string{}} }

// reset forgets all tabs (Chromium is being restarted).
func (t *tabs) reset() {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.byApp = map[string]string{}
	if t.p != nil {
		t.p.c.Close()
		t.p = nil
	}
}

// conn returns a live DevTools connection, retrying until timeout (Chromium
// may still be starting).
func (t *tabs) conn(timeout time.Duration) (*cdp, error) {
	deadline := time.Now().Add(timeout)
	for {
		if t.p != nil && !t.p.dead {
			return t.p, nil
		}
		p, err := dialCDP()
		if err == nil {
			t.p = p
			return p, nil
		}
		if time.Now().After(deadline) {
			return nil, fmt.Errorf("devtools: %v", err)
		}
		time.Sleep(time.Second)
	}
}

func (t *tabs) pages(p *cdp) ([]target, error) {
	var r struct{ TargetInfos []target }
	if err := p.call("Target.getTargets", map[string]any{}, "", &r); err != nil {
		return nil, err
	}
	var out []target
	for _, x := range r.TargetInfos {
		if x.Type == "page" {
			out = append(out, x)
		}
	}
	return out, nil
}

// focus brings the app's tab to the front, creating it if needed. On a fresh
// start Chromium has exactly one tab (opened on the app's URL): adopt it.
func (t *tabs) focus(a App, timeout time.Duration) error {
	t.mu.Lock()
	defer t.mu.Unlock()
	p, err := t.conn(timeout)
	if err != nil {
		return err
	}
	pages, err := t.pages(p)
	if err != nil {
		return err
	}
	exists := map[string]bool{}
	for _, x := range pages {
		exists[x.TargetID] = true
	}
	for app, id := range t.byApp {
		if !exists[id] {
			delete(t.byApp, app)
		}
	}
	id, ok := t.byApp[a.ID]
	fresh := !ok
	if !ok {
		mapped := map[string]bool{}
		for _, v := range t.byApp {
			mapped[v] = true
		}
		for _, x := range pages { // an unclaimed tab: the start-up tab, or a dropped app's spare
			if !mapped[x.TargetID] {
				id = x.TargetID
				if err := t.navigate(p, id, a.URL, x.URL); err != nil {
					return err
				}
				break
			}
		}
		if id == "" {
			var r struct{ TargetID string }
			if err := p.call("Target.createTarget", map[string]any{"url": a.URL}, "", &r); err != nil {
				return err
			}
			id = r.TargetID
		}
		t.byApp[a.ID] = id
	}
	log.Printf("tabs: %s -> %s", a.ID, id)
	if err := p.call("Target.activateTarget", map[string]any{"targetId": id}, "", nil); err != nil {
		return err
	}
	if fresh { // don't hand over (the tile closes) on a blank, still-loading page
		t.waitLoaded(p, id, 20*time.Second)
	}
	return nil
}

// waitLoaded polls the tab's document.readyState until "complete".
func (t *tabs) waitLoaded(p *cdp, id string, timeout time.Duration) {
	deadline := time.Now().Add(timeout)
	t.inTab(p, id, func(sid string) error {
		for time.Now().Before(deadline) {
			var r struct{ Result struct{ Value string } }
			err := p.call("Runtime.evaluate", map[string]any{"expression": "document.readyState", "returnByValue": true}, sid, &r)
			if err == nil && r.Result.Value == "complete" {
				return nil
			}
			time.Sleep(300 * time.Millisecond)
		}
		return nil
	})
}

// navigate loads to in a tab unless it already shows that site (the start-up
// tab was opened on the app's URL, possibly redirected since).
func (t *tabs) navigate(p *cdp, id, to, cur string) error {
	a, err1 := url.Parse(to)
	b, err2 := url.Parse(cur)
	if err1 == nil && err2 == nil && a.Scheme == b.Scheme && a.Host == b.Host && a.Host != "" {
		return nil
	}
	return t.inTab(p, id, func(sid string) error {
		return p.call("Page.navigate", map[string]any{"url": to}, sid, nil)
	})
}

func (t *tabs) inTab(p *cdp, id string, f func(sid string) error) error {
	var r struct{ SessionID string }
	if err := p.call("Target.attachToTarget", map[string]any{"targetId": id, "flatten": true}, "", &r); err != nil {
		return err
	}
	defer p.call("Target.detachFromTarget", map[string]any{"sessionId": r.SessionID}, "", nil)
	return f(r.SessionID)
}

// pauseAll pauses media in every tab.
func (t *tabs) pauseAll() {
	t.mu.Lock()
	defer t.mu.Unlock()
	p, err := t.conn(0)
	if err != nil {
		return
	}
	pages, _ := t.pages(p)
	for _, x := range pages {
		t.inTab(p, x.TargetID, func(sid string) error {
			return p.call("Runtime.evaluate", map[string]any{"expression": pauseJS}, sid, nil)
		})
	}
}

// drop closes the given apps' tabs (e.g. camera views that shouldn't keep
// streaming). The last tab is kept as a blank spare: closing it would quit
// Chromium.
func (t *tabs) drop(apps []string) {
	t.mu.Lock()
	defer t.mu.Unlock()
	p, err := t.conn(0)
	if err != nil {
		return
	}
	for _, app := range apps {
		id, ok := t.byApp[app]
		if !ok {
			continue
		}
		delete(t.byApp, app)
		pages, _ := t.pages(p)
		if len(pages) > 1 {
			p.call("Target.closeTarget", map[string]any{"targetId": id}, "", nil)
		} else {
			t.navigate(p, id, "about:blank", "")
		}
		log.Printf("tabs: dropped %s", app)
	}
}
