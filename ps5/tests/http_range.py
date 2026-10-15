#!/usr/bin/env python3
"""A tiny HTTP server with Range requests (as real web servers have), for the http:// tests.

  http_range.py <dir> <port>
"""
import http.server, os, sys

root, port = sys.argv[1], int(sys.argv[2])


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=root, **k)

    def send_head(self):
        path = self.translate_path(self.path)
        if not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        rng = self.headers.get('Range')
        f = open(path, 'rb')
        if rng and rng.startswith('bytes='):
            a, _, b = rng[6:].partition('-')
            start = int(a) if a else 0
            end = int(b) if b else size - 1
            end = min(end, size - 1)
            if start >= size:
                self.send_response(416)
                self.send_header('Content-Range', 'bytes */%d' % size)
                self.end_headers()
                f.close()
                return None
            f.seek(start)
            self.send_response(206)
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
            self.send_header('Content-Length', str(end - start + 1))
        else:
            self.send_response(200)
            self.send_header('Content-Length', str(size))
        self.send_header('Content-Type', 'application/octet-stream')
        self.send_header('Accept-Ranges', 'bytes')
        self.end_headers()
        return f

    def copyfile(self, src, dst):
        rng = self.headers.get('Range')
        if rng:
            a, _, b = rng[6:].partition('-')
            size = os.fstat(src.fileno()).st_size
            end = min(int(b) if b else size - 1, size - 1)
            left = end - src.tell() + 1
            while left > 0:
                d = src.read(min(65536, left))
                if not d:
                    break
                dst.write(d)
                left -= len(d)
        else:
            super().copyfile(src, dst)

    def log_message(self, *a):
        pass


http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
