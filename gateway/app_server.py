#!/usr/bin/env python3
"""Serves the teacher app copy (~/buddyband-app) on port 8080 for buddyband-app.service.

Like `python3 -m http.server`, but with `Cache-Control: no-cache`: phones must
re-check every file, otherwise they mix a new index.html with an old cached
style.css/app.js after a deploy (seen 17 Nov on the hotspot phone).
"""
import functools
import http.server
import sys
from pathlib import Path


class NoCacheHandler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()

    def log_message(self, fmt, *args):  # keep the journal quiet
        pass


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else Path.home() / "buddyband-app")
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    handler = functools.partial(NoCacheHandler, directory=str(root))
    http.server.ThreadingHTTPServer(("0.0.0.0", port), handler).serve_forever()


if __name__ == "__main__":
    main()
