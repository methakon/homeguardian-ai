#!/usr/bin/env python3
"""Diagnose premature POST dispatch against an already-running native demo.

Usage: python3 reproduce_fragmented_post.py PORT
Exit 1 means the defect reproduced; exit 0 means the server waited for the body.
No credentials are needed or sent. Only loopback connections are supported.
"""
import argparse
import socket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port', type=int)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('port must be between 1 and 65535')
    authority = f'127.0.0.1:{args.port}'
    headers = (
        'POST /api/session HTTP/1.1\r\n'
        f'Host: {authority}\r\n'
        f'Origin: http://{authority}\r\n'
        'Content-Type: application/json\r\n'
        'Content-Length: 2\r\n'
        'Connection: close\r\n\r\n'
    )
    with socket.create_connection(('127.0.0.1', args.port), timeout=3) as conn:
        conn.sendall(headers.encode('ascii'))
        conn.settimeout(0.5)
        try:
            early = conn.recv(4096)
        except socket.timeout:
            # A correct receiver waits for these body bytes before dispatching.
            conn.sendall(b'{}')
            conn.settimeout(3)
            response = conn.recv(4096)
            if not response.startswith(b'HTTP/1.'):
                raise RuntimeError('no HTTP response after completing the request')
            print('PASS: server waited for the declared body before responding')
            return 0
        if early.startswith(b'HTTP/1.1 400 '):
            print('DEFECT REPRODUCED: HTTP 400 sent before declared POST body arrived')
            return 1
        raise RuntimeError('unexpected early response; inspect native server behavior')


if __name__ == '__main__':
    raise SystemExit(main())
