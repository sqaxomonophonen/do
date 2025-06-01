// go build dojo.go

// TODO internet server:
//  - runs headless do-instances, one per "venue", and proxies http/ws
//    communication to/from them
//  - maintains per-venue journal/snapshots on disk
//  - stops idle do-instances, restarts them on demand?
//  - serves do.wasm/js build with audio engine enabled
//  - serves /dok/* - the file store

// required setup?
//  - do-binary; headless; only "logic"; no audio engine; no /dok/
//  - do-webpack: with audio engine
//  - journal/snapshot base directory
//  - bind
//  - dok dir set (one is read/write, others are read-only/shadows)
// optional setup?
//  - TLS keys? (for https/wss)

package main

import (
	"log"
	"net/http"
	"os"
	"path"
)

func main() {
	if len(os.Args) != 2 {
		log.Printf("Usage: %s <bind>\n", os.Args[0])
		os.Exit(1)
	}

	fs := http.FileServer(http.Dir(path.Dir(os.Args[0])))
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		log.Printf("%s %s", r.Method, r.URL.Path)

		/*
		// "no-cache": don't want to deal with caching problems (also, force
		// refreshes don't work too reliably with XHR calls, like the code that
		// loads the .wasm file...)
		w.Header().Add("Cache-Control", "no-cache, no-store, must-revalidate")
		*/

		// COOP/COEP headers provide 'crossOriginIsolated', required by
		// SharedArrayBuffer. probably not required by all served files, but
		// whatever.
		w.Header().Add("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Add("Cross-Origin-Embedder-Policy", "require-corp")

		fs.ServeHTTP(w, r)
	})

	log.Printf("HTTP on %s", os.Args[1])
	log.Fatal(http.ListenAndServe(os.Args[1], nil))
}
