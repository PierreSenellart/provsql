import http.server
import socketserver

# Dev server for the static browser build. It does nothing a plain static host
# (e.g. Apache, no CGI) could not: it just serves files with the right MIME
# types (notably application/wasm). The build is path-portable and uses only
# relative URLs, so no rewriting/redirects are needed; / serves the landing
# page (index.html), which links to the app (app.html).
http.server.SimpleHTTPRequestHandler.extensions_map.update({
    '.wasm': 'application/wasm', '.js': 'text/javascript',
    '.mjs': 'text/javascript', '.data': 'application/octet-stream',
    '.tar.gz': 'application/gzip',
})


class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


# Threaded: a browser opens many parallel/keep-alive connections (WASM,
# Pyodide, fonts, /api/* fetches). A single-threaded server serialises them
# and wedges on the first long-lived connection.
class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


with Server(('0.0.0.0', 8089), H) as httpd:
    print('serving on 0.0.0.0:8089')
    httpd.serve_forever()
