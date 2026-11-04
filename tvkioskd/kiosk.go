package main

import (
	"bufio"
	"bytes"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

const chroot = "/tmp/kiosk/root" // where kiosk-tv.sh mounts the image

// chromiumRunning: is Chromium's browser process alive inside the chroot?
func chromiumRunning() bool {
	ents, _ := os.ReadDir("/proc")
	for _, e := range ents {
		if _, err := strconv.Atoi(e.Name()); err != nil {
			continue
		}
		if r, _ := os.Readlink("/proc/" + e.Name() + "/root"); r != chroot {
			continue
		}
		cmd, _ := os.ReadFile("/proc/" + e.Name() + "/cmdline")
		if bytes.HasPrefix(cmd, []byte("/usr/lib/chromium/chromium\x00")) && !bytes.Contains(cmd, []byte("--type=")) {
			return true
		}
	}
	return false
}

// kioskEnv reads kiosk.env (KEY=VALUE lines) for kiosk-tv.sh.
func kioskEnv(dir string) []string {
	env := []string{"KIOSK_LEAVE=" + listenAddr}
	f, err := os.Open(filepath.Join(dir, "kiosk.env"))
	if err != nil {
		return env
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		l := strings.TrimSpace(sc.Text())
		if l != "" && !strings.HasPrefix(l, "#") && strings.Contains(l, "=") {
			env = append(env, l)
		}
	}
	return env
}

// kiosk runs kiosk-tv.sh from the stick with bash reading it on stdin (the
// TV's bash won't run script files from USB storage).
func kiosk(dir string, args ...string) error {
	script, err := os.ReadFile(filepath.Join(dir, "kiosk-tv.sh"))
	if err != nil {
		return err
	}
	cmd := exec.Command("bash", append([]string{"-s", "--"}, args...)...)
	cmd.Stdin = bytes.NewReader(script)
	cmd.Env = append(os.Environ(), kioskEnv(dir)...)
	out, err := cmd.CombinedOutput()
	log.Printf("kiosk %s: %s", strings.Join(args, " "), strings.TrimSpace(string(out)))
	if err != nil {
		return fmt.Errorf("kiosk %s: %v", args[0], err)
	}
	return nil
}

// startKiosk starts the kiosk on url, first clearing any leftovers (e.g. the
// bridge and Xvfb still up after Chromium was killed for memory).
func startKiosk(dir, url string) error {
	kiosk(dir, "stop")
	return kiosk(dir, "start", url)
}

// waitForStore finds kiosk/ on the partition labelled tvchrome once the TV has
// mounted it (at boot this can take a while).
func waitForStore(max time.Duration) string {
	deadline := time.Now().Add(max)
	for {
		if dev, err := exec.Command("blkid", "-L", "tvchrome").Output(); err == nil {
			dev = bytes.TrimSpace(dev)
			mounts, _ := os.ReadFile("/proc/mounts")
			for _, l := range strings.Split(string(mounts), "\n") {
				f := strings.Fields(l)
				if len(f) > 1 && f[0] == string(dev) && strings.HasPrefix(f[1], "/opt/media/") {
					if _, err := os.Stat(filepath.Join(f[1], "kiosk", "kiosk-tv.sh")); err == nil {
						return filepath.Join(f[1], "kiosk")
					}
				}
			}
		}
		if time.Now().After(deadline) {
			log.Fatal("no kiosk/kiosk-tv.sh on a partition labelled tvchrome")
		}
		time.Sleep(5 * time.Second)
	}
}
