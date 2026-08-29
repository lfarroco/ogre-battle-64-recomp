# WebAssembly Port — Browser Deployment

The Emscripten build uses pthreads, which requires `SharedArrayBuffer`. Browsers
only expose `SharedArrayBuffer` on **secure contexts** (HTTPS or localhost) with
**cross-origin isolation** headers.

## Required HTTP headers

```text
Cross-Origin-Opener-Policy: same-origin
Cross-Origin-Embedder-Policy: require-corp
```

Without these, the wasm module fails to start with an error such as:

```text
TypeError: Cannot create SharedArrayBuffer
```

(or a pthread-init failure in the browser console).

## Local testing

### Python

```sh
python3 - <<'EOF'
from http.server import SimpleHTTPRequestHandler, HTTPServer
class H(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()
HTTPServer(("localhost", 8080), H).serve_forever()
EOF
# then open http://localhost:8080/app/web/
```

### nginx

```nginx
add_header Cross-Origin-Opener-Policy "same-origin" always;
add_header Cross-Origin-Embedder-Policy "require-corp" always;
```

### Cloudflare Pages / Netlify

Add the two headers to the platform's custom headers configuration.

## File layout to serve

Serve the repo root (or a staging dir containing `app/web/` plus the wasm
output). The page references:

```text
app/web/index.html
app/web/web.js
build-wasm/ogrebattle64.js      (emscripten glue)
build-wasm/ogrebattle64.wasm
build-wasm/ogrebattle64.worker.js  (pthread workers)
```

## ROM handling

- The ROM is selected via the browser File API and read locally.
- It is written into the wasm virtual filesystem (MEMFS) — nothing is
  uploaded to any server.
- No copyrighted ROM data is bundled with the build.
