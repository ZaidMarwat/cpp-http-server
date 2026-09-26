#include "router.h"

#include <set>
#include <stdexcept>

namespace httpd {

// "/a/b" -> {"a", "b"}; "/" -> {""}; "/a/" -> {"a", ""}.
std::vector<std::string> Router::split(const std::string& path) {
    std::vector<std::string> out;
    size_t start = 1;
    for (;;) {
        size_t slash = path.find('/', start);
        out.push_back(path.substr(start, slash == std::string::npos ? std::string::npos : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return out;
}

Router& Router::add(std::string method, const std::string& pattern, Handler handler) {
    if (pattern.empty() || pattern[0] != '/') throw std::invalid_argument("route pattern must start with '/'");
    Route route{std::move(method), {}, std::move(handler)};
    for (const std::string& seg : split(pattern)) {
        bool param = seg.size() > 2 && seg.front() == '{' && seg.back() == '}';
        route.segments.push_back({param ? seg.substr(1, seg.size() - 2) : seg, param});
    }
    routes_.push_back(std::move(route));
    return *this;
}

bool Router::match(const Route& route, const std::vector<std::string>& segs) {
    if (segs.size() != route.segments.size()) return false;
    for (size_t i = 0; i < segs.size(); ++i) {
        const Segment& s = route.segments[i];
        if (s.param) {
            if (segs[i].empty()) return false;  // a parameter needs a value
        } else if (s.text != segs[i]) {
            return false;
        }
    }
    return true;
}

bool Router::more_specific(const Route& a, const Route& b) {
    for (size_t i = 0; i < a.segments.size(); ++i) {
        if (a.segments[i].param != b.segments[i].param) return !a.segments[i].param;
    }
    return false;
}

Response Router::dispatch(Request& req) const {
    // Split on the raw path first, then decode each segment, so an encoded
    // '/' (%2F) stays inside its segment instead of creating a new one.
    std::vector<std::string> raw = split(req.raw_path);
    std::vector<std::string> decoded;
    decoded.reserve(raw.size());
    for (const auto& s : raw) decoded.push_back(url_decode(s, false));

    const Route* best = nullptr;
    const Route* best_get = nullptr;  // for HEAD fallback
    std::set<std::string> allowed;

    for (const Route& r : routes_) {
        if (!match(r, decoded)) continue;
        allowed.insert(r.method);
        if (r.method == req.method && (!best || more_specific(r, *best))) best = &r;
        if (r.method == "GET" && (!best_get || more_specific(r, *best_get))) best_get = &r;
    }

    if (!best && req.method == "HEAD") best = best_get;

    if (!best) {
        if (allowed.empty()) return Response::text(404, "not found: " + req.path + "\n");
        if (allowed.count("GET")) allowed.insert("HEAD");
        std::string allow;
        for (const auto& m : allowed) allow += (allow.empty() ? "" : ", ") + m;
        Response r = Response::text(405, "method " + req.method + " not allowed on " + req.path + "\n");
        r.headers["allow"] = allow;
        return r;
    }

    req.params.clear();
    for (size_t i = 0; i < best->segments.size(); ++i)
        if (best->segments[i].param) req.params[best->segments[i].text] = decoded[i];
    return best->handler(req);
}

}  // namespace httpd
