package main

import (
	"bufio"
	"log"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

// takeover replaces a TV app (by default the stock web browser) with the
// kiosk's browser: when that app comes on screen, it is closed and the kiosk
// opens on its start page instead.
//
// The app manager (AUL) announces launches and status changes as D-Bus signals
// on org.tizen.aul.AppStatus; any signal naming the app triggers a check of
// its status with aul_test. If dbus-monitor can't run, the status is polled.
func (d *daemon) takeover(appid string) {
	check := func() {
		if appStatus(appid) != statusVisible {
			return
		}
		log.Printf("takeover: %s is on screen, opening the kiosk browser instead", appid)
		exec.Command("app_launcher", "-t", appid).Run()
		if a, ok := d.config().app("browser"); ok {
			if err := d.open(a, ""); err != nil {
				log.Printf("takeover: %v", err)
			}
		}
	}
	for {
		cmd := exec.Command("dbus-monitor", "--system", "type='signal',interface='org.tizen.aul.AppStatus'")
		out, err := cmd.StdoutPipe()
		if err == nil {
			err = cmd.Start()
		}
		if err != nil {
			log.Printf("takeover: dbus-monitor: %v; polling instead", err)
			for {
				check()
				time.Sleep(2 * time.Second)
			}
		}
		log.Printf("takeover: watching for %s", appid)
		sc := bufio.NewScanner(out)
		for sc.Scan() {
			// Arguments follow the signal's header line; the app id shows up
			// as a quoted string argument.
			if strings.Contains(sc.Text(), `"`+appid+`"`) {
				time.Sleep(300 * time.Millisecond) // let the status settle
				check()
			}
		}
		cmd.Wait()
		log.Print("takeover: dbus-monitor exited, restarting")
		time.Sleep(5 * time.Second)
	}
}

// AUL app status values (aul.h): 3 = visible (on screen), 4 = background.
const statusVisible = 3

// appStatus reads an app's AUL status from aul_test; -1 if it isn't running.
func appStatus(appid string) int {
	out, err := exec.Command("aul_test", "getallappstatus").Output()
	if err != nil {
		return -1
	}
	return parseAppStatus(string(out), appid)
}

func parseAppStatus(out, appid string) int {
	cur := ""
	for _, l := range strings.Split(out, "\n") {
		f := strings.SplitN(strings.TrimSpace(l), ":", 2)
		if len(f) != 2 {
			continue
		}
		k, v := strings.TrimSpace(f[0]), strings.TrimSpace(f[1])
		switch k {
		case "appid":
			cur = v
		case "status":
			if cur == appid {
				n, err := strconv.Atoi(v)
				if err != nil {
					return -1
				}
				return n
			}
		}
	}
	return -1
}
