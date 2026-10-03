# cpp-http-server

An HTTP/1.1 server written from scratch in C++17 over raw POSIX sockets: the
request parser, router, event loop and worker pool are all here, with no
dependencies beyond the standard library. It builds and runs on macOS and Linux.

```
          accept / read / parse / write               run handlers
        ┌──────────────────────────────────┐   job   ┌──────────────┐
client ─┤ event loop: poll() over          ├───────► │ thread pool  │
client ─┤ non-blocking sockets             │         │ (N workers)  │
client ─┤ timeouts, keep-alive, pipelining │ ◄───────┤              │
        └──────────────────────────────────┘   wake  └──────────────┘
                                               pipe
```

One thread does all the socket I/O. A connection only borrows a worker while
its handler is actually running, so thousands of idle keep-alive connections
cost no threads, and a slow or silent client can't starve the pool. (A
thread-per-connection design with 8 workers stops serving entirely once 8
clients open a connection and go quiet. That case is covered by a test here.)

## Features

- **Incremental parser.** Accepts bytes in any split, down to one at a time.
  It handles `Content-Length` and **chunked** bodies (extensions and trailers
  included), pipelined requests, and `Expect: 100-continue`.
- **Strict framing.** `Content-Length` must be plain digits, and duplicate
  lengths must agree. `Content-Length` together with `Transfer-Encoding` is
  rejected, as are whitespace before a header colon, obsolete line folding, and
  bare CR/LF/NUL. These are the ambiguities that request-smuggling attacks
  exploit.
- **Accurate error statuses:** 400, 413, 414, 417, 431, 501 and 505. Every size
  limit is configurable.
- **Router.** `{param}` segments are decoded per segment, so `/files/a%2Fb`
  gives `name = "a/b"`. The most specific route wins, so `/users/me` beats
  `/users/{id}` in either registration order. Unknown methods get **405** with
  an `Allow` header, and **HEAD** falls back to GET with the body dropped.
- **Connection handling.** Keep-alive follows the HTTP version (1.0 closes by
  default). There are idle and slow-request timeouts (**408**), and a client
  that half-closes still gets its answer. When the process runs out of file
  descriptors it sheds clients instead of spinning.
- **Safety.** A handler that throws becomes a **500**, and the server keeps
  running. `Ctrl-C` shuts down cleanly through an async-signal-safe `stop()`.

## Build and test

```sh
make              # ./server, build/tests, build/bench
make check        # 146 checks: parser, router, responses, end-to-end over sockets
make check-asan   # same tests under AddressSanitizer + UndefinedBehaviorSanitizer
make check-tsan   # same tests under ThreadSanitizer
```

## Run

```sh
./server 8080 4                                  # port, worker threads
curl localhost:8080/health                       # {"status":"ok"}
curl "localhost:8080/echo?a=1&b=two+words"       # a=1 / b=two words
curl localhost:8080/users/42                     # {"id":"42"}
curl localhost:8080/users/me                     # literal route wins
curl -d '1 2 3' localhost:8080/sum               # 6
curl -X DELETE -i localhost:8080/health          # 405, Allow: GET, HEAD
curl -H 'Transfer-Encoding: chunked' --data-binary @README.md localhost:8080/echo
```

## Using it as a library

```cpp
#include "server.h"

httpd::Router router;
router.get("/hello/{name}", [](const httpd::Request& req) {
    return httpd::Response::text(200, "hello " + req.params.at("name") + "\n");
});

httpd::ServerOptions opts;
opts.port = 8080;
opts.workers = 4;
httpd::Server server(std::move(router), opts);
server.run();  // until server.stop()
```

## Benchmark

```sh
./server 18123 4 &
./build/bench 18123 /bench 64 3     # path, connections, seconds
```

The figures below are for an Apple M1 over loopback, with a 3-byte response
and 4 workers:

| connections | requests/s | p50     | p99     | p99.9   |
|-------------|-----------:|---------|---------|---------|
| 8           | ~99,000    | 0.08 ms | 0.13 ms | 0.16 ms |
| 64          | ~152,000   | 0.38 ms | 0.80 ms | 0.90 ms |

These are best-case numbers for tiny responses on one machine, not a claim
about real traffic. The point of the benchmark is to have a repeatable harness
for measuring changes.

## Layout

```
src/http_types.*      Request, Response, status text, URL/JSON helpers
src/request_parser.*  incremental, strict HTTP/1.1 request parser
src/router.*          method + pattern routing, 404/405/HEAD
src/thread_pool.h     fixed-size worker pool
src/server.*          poll() event loop, timeouts, keep-alive, pipelining
src/main.cpp          demo server
tests/test_main.cpp   test suite
bench/bench.cpp       closed-loop load generator with latency percentiles
```

## Limits (on purpose)

- HTTP/1.1 only, with no TLS. Put a TLS-terminating proxy in front.
- IPv4 only.
- `poll()` scans every connection on each turn. That's fine for thousands of
  connections. For hundreds of thousands, swap in epoll or kqueue; the rest of
  the server doesn't change.
- Responses are built in memory. There's no streaming or chunked *responses*.
