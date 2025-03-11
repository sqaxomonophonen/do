// go build temporary_httpd.go

// NOTE *** this is temporary *** at some point I want to serve the html5 app
// from a httpd in the native app. but that's significantly harder to do than a
// small temporary golang hack.

package main

import (
	"log"
	"net/http"
	"os"
	"path"
)

func main() {
	fs := http.FileServer(http.Dir(path.Dir(os.Args[0])))
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		log.Printf("%s %s", r.Method, r.URL.Path)

		// "no-cache": don't want to deal with caching problems (also, force
		// refreshes don't work too reliably with XHR calls, like the code that
		// loads the .wasm file...)
		w.Header().Add("Cache-Control", "no-cache, no-store, must-revalidate")

		// COOP/COEP headers required by SharedArrayBuffer. probably not
		// required by all served files, but whatever.
		w.Header().Add("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Add("Cross-Origin-Embedder-Policy", "require-corp")

		fs.ServeHTTP(w, r)
	})

	bind := ":6581"
	log.Printf("HTTP on %s", bind)
	log.Fatal(http.ListenAndServe(bind, nil))
}
