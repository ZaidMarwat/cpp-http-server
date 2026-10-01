// Demo server.
//
//   GET  /              plain-text landing page
//   GET  /health        {"status":"ok"}
//   GET  /echo?a=b      echoes the decoded query parameters
//   POST /echo          echoes the request body (works with chunked uploads)
//   GET  /users/me      literal route that beats /users/{id}
//   GET  /users/{id}    path parameter, JSON-escaped
//   POST /sum           body "1 2 3" -> 6; 400 on anything that isn't integers
//   GET  /bench         tiny fixed body for the load benchmark
//
// usage: server [port] [workers]

#include <charconv>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "server.h"

using httpd::Request;
using httpd::Response;

static httpd::Server* g_server = nullptr;

extern "C" void on_signal(int) {
    if (g_server) g_server->stop();  // async-signal-safe
}

int main(int argc, char** argv) {
    httpd::ServerOptions opts;
    opts.port = argc > 1 ? std::atoi(argv[1]) : 8080;
    opts.workers = argc > 2 ? static_cast<size_t>(std::atoi(argv[2])) : 4;

    httpd::Router router;

    router.get("/", [](const Request&) {
        return Response::text(200, "cpp-http-server: see README.md\n");
    });

    router.get("/health", [](const Request&) {
        return Response::json(200, "{\"status\":\"ok\"}\n");
    });

    router.get("/echo", [](const Request& req) {
        std::string out;
        for (const auto& [k, v] : req.query) out += k + "=" + v + "\n";
        return Response::text(200, out);
    });

    router.post("/echo", [](const Request& req) {
        Response r = Response::text(200, req.body);
        if (const std::string* type = req.header("content-type")) r.headers["content-type"] = *type;
        return r;
    });

    router.get("/users/me", [](const Request&) {
        return Response::json(200, "{\"id\":\"me\",\"note\":\"literal route\"}\n");
    });

    router.get("/users/{id}", [](const Request& req) {
        return Response::json(200, "{\"id\":\"" + httpd::json_escape(req.params.at("id")) + "\"}\n");
    });

    router.post("/sum", [](const Request& req) {
        std::istringstream in(req.body);
        std::string token;
        long long total = 0;
        while (in >> token) {
            long long n = 0;
            auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), n);
            if (ec != std::errc() || end != token.data() + token.size() ||
                __builtin_add_overflow(total, n, &total))
                return Response::text(400, "body must be whitespace-separated integers (64-bit)\n");
        }
        return Response::text(200, std::to_string(total) + "\n");
    });

    router.get("/bench", [](const Request&) {
        return Response::text(200, "ok\n");
    });

    try {
        httpd::Server server(std::move(router), opts);
        g_server = &server;
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        std::signal(SIGPIPE, SIG_IGN);

        std::cout << "listening on http://" << opts.host << ":" << server.port() << " with " << opts.workers
                  << " workers (Ctrl-C to stop)" << std::endl;
        server.run();
        g_server = nullptr;
        std::cout << "shutting down" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
