// ItemCmd - small helper for the ItemDrop plugin. Starts a tiny local web page (127.0.0.1 only,
// nothing leaves your PC), opens it in your browser and exits when the tab is closed.
package main

import (
	_ "embed"
	"fmt"
	"net"
	"net/http"
	"os/exec"
	"runtime"
	"sync/atomic"
	"time"
	"os"
)

//go:embed index.html
var page []byte

func main() {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Println("cannot start:", err)
		time.Sleep(5 * time.Second)
		os.Exit(1)
	}
	url := "http://" + ln.Addr().String() + "/"

	var lastPing atomic.Int64
	lastPing.Store(time.Now().Unix())
	var seenPing atomic.Bool

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		w.Write(page)
	})
	mux.HandleFunc("/ping", func(w http.ResponseWriter, r *http.Request) {
		lastPing.Store(time.Now().Unix())
		seenPing.Store(true)
		w.WriteHeader(http.StatusNoContent)
	})

	go http.Serve(ln, mux)

	fmt.Println("ItemCmd is running at", url)
	fmt.Println("Close the browser tab (or this window) to quit.")
	if runtime.GOOS == "windows" {
		exec.Command("rundll32", "url.dll,FileProtocolHandler", url).Start()
	}

	for {
		time.Sleep(2 * time.Second)
		idle := time.Now().Unix() - lastPing.Load()
		// before the first ping give the browser 60 s to open; afterwards 8 s without ping = tab closed
		if (seenPing.Load() && idle > 8) || idle > 60 {
			return
		}
	}
}
