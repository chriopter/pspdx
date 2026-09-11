#!/usr/bin/env python3
"""One directory over TLS 1.3 on the loopback address, which is all a catalog
on the host has to be. The client is built to trust the CA that signed this
certificate and nothing else, so nothing outside the work directory is
reachable from a build that talks to it.

    serve.py <site dir> <cert> <key> [port]
"""
import http.server, os, ssl, sys

site, cert, key = sys.argv[1], sys.argv[2], sys.argv[3]
port = int(sys.argv[4]) if len(sys.argv) > 4 else 8443
os.chdir(site)

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(cert, key)


class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a):
        pass


srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Quiet)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
srv.serve_forever()
