package main

import "testing"

// Output of `aul_test getallappstatus` on the TV (Tizen 3.0), shortened.
const aulSample = `[aul_app_get_all_running_app_info 0 test] 
	==========================
	 pid: 342
	 appid: org.tizen.tv-viewer
	 app_path: /usr/apps/org.tizen.tv-viewer/bin/tv-viewer
	 pkgid: org.tizen.tv-viewer
	 status: 3
	 is_sub_app : 0
	==========================
	==========================
	 pid: 1172
	 appid: org.tizen.browser
	 app_path: /usr/apps/org.tizen.browser/bin/browser
	 pkgid: org.tizen.browser
	 status: 4
	 is_sub_app : 0
	==========================
`

func TestParseAppStatus(t *testing.T) {
	for app, want := range map[string]int{"org.tizen.tv-viewer": 3, "org.tizen.browser": 4, "org.example.none": -1} {
		if got := parseAppStatus(aulSample, app); got != want {
			t.Errorf("%s: got %d, want %d", app, got, want)
		}
	}
}
