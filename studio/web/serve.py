import http.server
import socketserver

# Dev server for the static browser build. It does nothing a plain static
# host (e.g. Apache, no CGI) could not: it serves files and issues two fixed
# redirects for the clean mode paths. The production equivalent is two lines:
#   Redirect /circuit /?mode=circuit
#   Redirect /where   /?mode=where
REDIRECTS = {'/circuit': '/?mode=circuit', '/where': '/?mode=where'}

http.server.SimpleHTTPRequestHandler.extensions_map.update({
    '.wasm': 'application/wasm', '.js': 'text/javascript',
    '.mjs': 'text/javascript', '.data': 'application/octet-stream',
    '.tar.gz': 'application/gzip',
})


class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def do_GET(self):
        target = REDIRECTS.get(self.path.split('?', 1)[0])
        if target is not None:
            self.send_response(302)
            self.send_header('Location', target)
            self.end_headers()
            return
        return super().do_GET()


# Threaded: a browser opens many parallel/keep-alive connections (WASM,
# Pyodide, fonts, /api/* fetches). A single-threaded server serialises them
# and wedges on the first long-lived connection.
class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


with Server(('0.0.0.0', 8089), H) as httpd:
    print('serving on 0.0.0.0:8089')
    httpd.serve_forever()
