#pragma once

// Method + path routing.
//
// Patterns are made of literal segments and {name} parameters, e.g.
// "/users/{id}/posts/{post}". Captured values land in Request::params,
// percent-decoded per segment (so "/files/a%2Fb" gives name = "a/b" instead
// of being split into two segments).
//
// When several routes match a path, the most specific wins, comparing
// segments left to right with literals beating parameters. So "/users/me"
// takes priority over "/users/{id}" no matter which was registered first.
//
// A path that exists under another method gets 405 with an Allow header.
// HEAD falls back to the GET handler (the server then drops the body).

#include <functional>
#include <string>
#include <vector>

#include "http_types.h"

namespace httpd {

using Handler = std::function<Response(const Request&)>;

class Router {
public:
    Router& add(std::string method, const std::string& pattern, Handler handler);

    Router& get(const std::string& pattern, Handler h) { return add("GET", pattern, std::move(h)); }
    Router& post(const std::string& pattern, Handler h) { return add("POST", pattern, std::move(h)); }
    Router& put(const std::string& pattern, Handler h) { return add("PUT", pattern, std::move(h)); }
    Router& del(const std::string& pattern, Handler h) { return add("DELETE", pattern, std::move(h)); }

    // Fills req.params and runs the matching handler, or returns 404 / 405.
    // Exceptions thrown by handlers propagate to the caller.
    Response dispatch(Request& req) const;

private:
    struct Segment {
        std::string text;  // literal text, or the parameter name
        bool param;
    };
    struct Route {
        std::string method;
        std::vector<Segment> segments;
        Handler handler;
    };
    std::vector<Route> routes_;

    static std::vector<std::string> split(const std::string& path);
    static bool match(const Route& route, const std::vector<std::string>& segs);
    static bool more_specific(const Route& a, const Route& b);
};

}  // namespace httpd
