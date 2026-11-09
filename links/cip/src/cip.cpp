// Copyright (c) 2026 AlexisVon
// SPDX-License-Identifier: Apache-2.0

#include "internal.h"

#include "ascript.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/// The script surface: open/listen return a handle (an anyptr, copies share the connection), and
/// the define/get/set verbs take it as the first argument.
namespace cip_link {

    namespace {

        using alx::script::error_type;
        using alx::script::fwrap;
        using alx::script::script_exception;

        [[noreturn]] void fail(const char* _cap, const std::string& _what, const std::string& _why) {
            throw script_exception{error_type::RuntimeError,
                                   std::string("cip.") + _cap + " " + _what + ": " + _why};
        }

        // Type-checked argument access; _desc is "<fn> <param>", and it goes into the error
        // verbatim, so the caller can see which argument was wrong.
        std::string as_str(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<std::string>())
                throw script_exception{error_type::TypeError, _desc + ": expected string"};
            return _v.as<std::string>();
        }

        alx::int_64 as_int(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::int_64>())
                throw script_exception{error_type::TypeError, _desc + ": expected int"};
            return _v.as<alx::int_64>();
        }

        alx::varvec& as_vec(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::varvec>())
                throw script_exception{error_type::TypeError, _desc + ": expected vec"};
            return _v.as<alx::varvec>();
        }

        alx::varmap& as_map(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::varmap>())
                throw script_exception{error_type::TypeError, _desc + ": expected map"};
            return _v.as<alx::varmap>();
        }

        // A non-handle must raise here: the cast dereferences the register tag.
        cip_conn* as_conn(alx::variant& _v, const char* _cap) {
            const std::string desc = std::string("cip.") + _cap + " handle";
            if (!_v.is<alx::anyptr>())
                throw script_exception{error_type::TypeError, desc + ": expected a handle"};
            cip_conn* c = alx::anyptr_ex<cip_conn>::as(_v.as<alx::anyptr>());
            if (!c) throw script_exception{error_type::TypeError, desc + ": handle type mismatch"};
            return c;
        }

        // The script's own truthiness, for what a condition's value means: a predicate that says
        // nothing (null) is a no.
        bool truthy(const alx::variant& _v) {
            if (_v.null()) return false;
            if (_v.is<bool>()) return _v.to<bool>();
            if (_v.is<alx::int_64>()) return _v.to<alx::int_64>() != 0;
            if (_v.is<double>()) return _v.to<double>() != 0;
            if (_v.is<std::string>()) return !_v.to<std::string>().empty();
            if (_v.is<alx::bytes>()) return _v.to<alx::bytes>().size() != 0;
            if (_v.is<alx::varvec>()) return !_v.to<alx::varvec>().empty();
            if (_v.is<alx::varmap>()) return !_v.to<alx::varmap>().empty();
            return true;
        }

        std::string text_of(const alx::variant& _v) {
            if (_v.null()) return "null";
            if (_v.is<bool>()) return _v.to<bool>() ? "true" : "false";
            if (_v.is<alx::int_64>()) return std::to_string(_v.to<alx::int_64>());
            if (_v.is<double>()) return std::to_string(_v.to<double>());
            if (_v.is<std::string>()) return "\"" + _v.to<std::string>() + "\"";
            if (_v.is<alx::varvec>()) return "a vec";
            if (_v.is<alx::bytes>()) return "bytes";
            return "?";
        }

        // Every declaration lands in the same table; on a server handle it also seeds the point's
        // value, so the wire answers from the first request on.
        void declare_point(cip_conn* _c, const std::string& _name, const point& _p, const char* _cap) {
            auto it = _c->points.find(_name);
            if (it != _c->points.end())
                fail(_cap, _name,
                     "already declared as " + describe(it->second) + " -- remove it first");
            _c->points.emplace(_name, _p);
            if (_c->server) _c->locals.emplace(_name, zero_of(_p));
        }

        // The shape argument of a define: an element count, or [rows, columns] for a 2-D point.
        // Row-major; the wire still sees one flat run of elements.
        void shape_of(alx::variant& _v, const std::string& _name, const char* _cap, point& _out) {
            if (_v.is<alx::varvec>()) {
                alx::varvec& dims = as_vec(_v, std::string("cip.") + _cap + " shape");
                if (dims.size() != 2)
                    fail(_cap, _name,
                         "a 2-D shape is [rows, columns] -- " + std::to_string(dims.size()) +
                             " dimensions is not spelled yet");
                const int64_t rows = as_int(dims[0], std::string("cip.") + _cap + " rows");
                const int64_t cols = as_int(dims[1], std::string("cip.") + _cap + " columns");
                if (rows < 1 || cols < 1 || rows * cols > 1000000)
                    fail(_cap, _name, "a " + std::to_string(rows) + "x" + std::to_string(cols) + " shape is not one this build declares");
                _out.dims[0] = static_cast<uint32_t>(rows);
                _out.dims[1] = static_cast<uint32_t>(cols);
                _out.count = static_cast<uint32_t>(rows * cols);
                return;
            }
            const int64_t count = as_int(_v, std::string("cip.") + _cap + " count");
            if (count < 1 || count > 1000000) fail(_cap, _name, "count must be 1 or more");
            _out.count = static_cast<uint32_t>(count);
        }

        // One declaration: name + type + shape. A duplicate name is refused, never overwritten --
        // a repeated declaration is a script mistake, and remove-then-define says a real change.
        void define_one(fwrap& _fw, const char* _cap, const cip_type& _t) {
            cip_conn* c = as_conn(_fw[0], _cap);
            const std::string& name = as_str(_fw[1], std::string("cip.") + _cap + " name");
            point p{&_t, 1};
            if (_fw.size() > 2) shape_of(_fw[2], name, _cap, p);
            std::lock_guard<std::mutex> lk(c->mtx);
            declare_point(c, name, p, _cap);
        }

        // A string declares no length: one value, which carries its own on the wire.
        void define_string_like(fwrap& _fw, const char* _cap, const cip_type* _t) {
            cip_conn* c = as_conn(_fw[0], _cap);
            const std::string& name = as_str(_fw[1], std::string("cip.") + _cap + " name");
            std::lock_guard<std::mutex> lk(c->mtx);
            declare_point(c, name, point{_t, 1}, _cap);
        }

        void fn_define_bool(fwrap& _fw) { define_one(_fw, "define_bool", kTypes[0]); }
        void fn_define_sint(fwrap& _fw) { define_one(_fw, "define_sint", kTypes[1]); }
        void fn_define_int(fwrap& _fw) { define_one(_fw, "define_int", kTypes[2]); }
        void fn_define_dint(fwrap& _fw) { define_one(_fw, "define_dint", kTypes[3]); }
        void fn_define_lint(fwrap& _fw) { define_one(_fw, "define_lint", kTypes[4]); }
        void fn_define_real(fwrap& _fw) { define_one(_fw, "define_real", kTypes[5]); }
        void fn_define_lreal(fwrap& _fw) { define_one(_fw, "define_lreal", kTypes[6]); }
        void fn_define_byte(fwrap& _fw) { define_one(_fw, "define_byte", *named("BYTE")); }
        void fn_define_string(fwrap& _fw) { define_string_like(_fw, "define_string", named("STRING")); }
        void fn_define_sstring(fwrap& _fw) { define_string_like(_fw, "define_sstring", named("SSTRING")); }

        void fn_remove(fwrap& _fw) {
            cip_conn* c = as_conn(_fw[0], "remove");
            const std::string& name = as_str(_fw[1], "cip.remove name");
            std::lock_guard<std::mutex> lk(c->mtx);
            if (c->points.erase(name) == 0) fail("remove", name, "not declared");
            c->locals.erase(name);
        }

        // The point table as the script's own record: {name: "DINT"}, {"Tag_Array": "INT[10]"}.
        void fn_points(fwrap& _fw) {
            cip_conn* c = as_conn(_fw[0], "points");
            std::lock_guard<std::mutex> lk(c->mtx);
            alx::varmap out;
            for (const auto& kv : c->points) out[kv.first] = alx::variant(describe(kv.second));
            _fw.freturn(alx::variant(std::move(out)));
        }

        // A failed get leaves the reason behind and hands back null; a failed set leaves it and
        // returns false. A failure only the script can fix raises instead (see read_tag).
        void remember(cip_conn* _c, std::string _reason) {
            std::lock_guard<std::mutex> lk(_c->mtx);
            _c->reason = std::move(_reason);
        }

        // A vec of names reads a batch: the values come back in the order the names were given,
        // and a point that is not there comes back null with its reason left in error().
        void fn_get(fwrap& _fw) {
            cip_conn* c = as_conn(_fw[0], "get");
            if (_fw[1].is<alx::varvec>()) {
                alx::varvec& tags = as_vec(_fw[1], "cip.get tags");
                alx::varvec out;
                std::string reason;
                for (alx::variant& name : tags) {
                    const std::string& tag = as_str(name, "cip.get tag");
                    std::string why;
                    bool script_err = false;
                    alx::variant v;
                    const bool ok = c->server ? local_read(c, tag, v, script_err, why)
                                              : read_tag(c, tag, v, script_err, why);
                    if (!ok) {
                        if (script_err) fail("get", tag, why);
                        reason = "cip.get " + tag + ": " + why;
                        v = alx::variant(); // null
                    }
                    out.push_back(std::move(v));
                }
                remember(c, std::move(reason));
                _fw.freturn(alx::variant(std::move(out)));
                return;
            }
            const std::string& tag = as_str(_fw[1], "cip.get tag");
            std::string why;
            bool script_err = false;
            alx::variant out;
            const bool ok = c->server ? local_read(c, tag, out, script_err, why)
                                      : read_tag(c, tag, out, script_err, why);
            if (!ok) {
                if (script_err) fail("get", tag, why);
                remember(c, "cip.get " + tag + ": " + why);
                _fw.freturn(alx::variant()); // null
                return;
            }
            remember(c, std::string());
            _fw.freturn(out);
        }

        // A write needs a declaration (the request carries its type); an undeclared point is a
        // refusal, not a throw. A tag that does not parse raises.
        bool write_declared(cip_conn* _c, const std::string& _tag, const alx::variant& _v,
                            std::string& _reason, bool& _script_err, std::string& _why) {
            point p;
            tag_ref ref;
            {
                std::lock_guard<std::mutex> lk(_c->mtx);
                if (!lookup_point(_c, _tag, ref, p, _script_err, _why) || !p.type) {
                    if (!_script_err) _why = "not declared -- a define line for it comes first";
                    _reason = "cip.set " + _tag + ": " + _why;
                    return false;
                }
            }
            if (!write_tag(_c, _tag, _v, p, _script_err, _why)) {
                _reason = "cip.set " + _tag + ": " + _why;
                return false;
            }
            return true;
        }

        // A map writes a batch; it stops at the first point that refuses (the ones before it went
        // through), leaving that reason in error().
        void fn_set(fwrap& _fw) {
            cip_conn* c = as_conn(_fw[0], "set");
            if (_fw[1].is<alx::varmap>()) {
                alx::varmap& points = as_map(_fw[1], "cip.set points");
                for (auto it = points.begin(); it != points.end(); ++it) {
                    std::string reason, why;
                    bool script_err = false;
                    const bool ok = c->server
                                        ? local_write(c, it.key(), it.value(), script_err, why)
                                        : write_declared(c, it.key(), it.value(), reason, script_err, why);
                    if (!ok) {
                        if (script_err) fail("set", it.key(), why);
                        if (c->server) reason = "cip.set " + it.key() + ": " + why;
                        remember(c, std::move(reason));
                        _fw.freturn(alx::variant(false));
                        return;
                    }
                }
                remember(c, std::string());
                _fw.freturn(alx::variant(true));
                return;
            }
            const std::string& tag = as_str(_fw[1], "cip.set tag");
            std::string reason, why;
            bool script_err = false;
            const bool ok = c->server
                                ? local_write(c, tag, _fw[2], script_err, why)
                                : write_declared(c, tag, _fw[2], reason, script_err, why);
            if (!ok) {
                if (script_err) fail("set", tag, why);
                if (c->server) reason = "cip.set " + tag + ": " + why;
                remember(c, std::move(reason));
                _fw.freturn(alx::variant(false));
                return;
            }
            remember(c, std::string());
            _fw.freturn(alx::variant(true));
        }

        // Wait for the script's condition to hold: a server handle on the remote's write events, a
        // client handle sampling on its period. False at the deadline, the last value in error().
        void fn_wait(fwrap& _fw) {
            cip_conn* c = as_conn(_fw[0], "wait");
            const bool listed = _fw[1].is<alx::varvec>();
            std::vector<std::string> keys;
            std::string what;
            if (listed) {
                alx::varvec& names = as_vec(_fw[1], "cip.wait keys");
                if (names.empty()) fail("wait", "keys", "the list is empty -- name a key");
                for (alx::variant& name : names) keys.push_back(as_str(name, "cip.wait key"));
                for (const std::string& key : keys) what += (what.empty() ? "" : ",") + key;
            } else {
                keys.push_back(as_str(_fw[1], "cip.wait key"));
                what = keys[0];
            }
            const std::string cond_name =
                _fw[2].is<std::string>() ? _fw[2].to<std::string>() : std::string("the condition");
            int64_t timeout_ms = as_int(_fw[3], "cip.wait timeout");
            int64_t period_ms = 20;
            if (_fw.size() > 4) period_ms = as_int(_fw[4], "cip.wait period");
            if (timeout_ms < 1) timeout_ms = 1;
            if (period_ms < 1) period_ms = 1;

            // One check: read the key(s), hand the value(s) to the condition. 1 holds, 0 does not,
            // -1 the check itself failed (the reason in _why) -- and _seen keeps what was read.
            auto evaluate = [&](alx::variant& _seen, std::string& _why) -> int {
                alx::variant got;
                if (keys.size() == 1) {
                    std::string why;
                    bool script_err = false;
                    const bool ok = c->server ? local_read_locked(c, keys[0], got, script_err, why)
                                              : read_tag(c, keys[0], got, script_err, why);
                    if (!ok) {
                        if (script_err) fail("wait", keys[0], why);
                        _why = "cip.wait " + keys[0] + ": " + why;
                        return -1;
                    }
                } else {
                    alx::varvec vals;
                    for (const std::string& key : keys) {
                        std::string why;
                        bool script_err = false;
                        alx::variant v;
                        const bool ok = c->server ? local_read_locked(c, key, v, script_err, why)
                                                  : read_tag(c, key, v, script_err, why);
                        if (!ok) {
                            if (script_err) fail("wait", key, why);
                            _why = "cip.wait " + key + ": " + why;
                            return -1;
                        }
                        vals.push_back(std::move(v));
                    }
                    got = alx::variant(std::move(vals));
                }
                _seen = got;
                alx::varvec args;
                args.push_back(std::move(got));
                alx::variant verdict;
                try {
                    // The callable is re-read each time: a call into the script invalidates the
                    // argument references the call object handed out.
                    verdict = _fw.call(_fw[2], args);
                } catch (const script_exception& _e) {
                    if (_e.type != error_type::NameError) throw; // the predicate's own
                    _why = "cip.wait " + what + ": '" + cond_name + "' is not callable";
                    return -1;
                }
                return truthy(verdict) ? 1 : 0;
            };

            if (c->server) { // the local table, woken by the remote's writes -- no sampling
                dbg("server wait enters: " + what);
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
                std::unique_lock<std::mutex> lk(c->mtx);
                for (;;) {
                    alx::variant seen;
                    std::string why;
                    const int verdict = evaluate(seen, why);
                    if (verdict == 1) {
                        c->reason.clear(); // the lock is held: write it directly, not via remember()
                        _fw.freturn(alx::variant(true));
                        return;
                    }
                    if (verdict < 0) {
                        c->reason = why;
                        _fw.freturn(alx::variant(false));
                        return;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) {
                        c->reason = "cip.wait " + what + ": still " + text_of(seen) + " after " +
                                    std::to_string(timeout_ms) + " ms";
                        _fw.freturn(alx::variant(false));
                        return;
                    }
                    dbg("server wait: sleeping until the deadline");
                    c->cv.wait_until(lk, deadline);
                    dbg("server wait: woke");
                }
            }

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            for (;;) {
                alx::variant seen;
                std::string why;
                const int verdict = evaluate(seen, why);
                if (verdict == 1) {
                    remember(c, std::string());
                    _fw.freturn(alx::variant(true));
                    return;
                }
                if (verdict < 0) {
                    remember(c, std::move(why));
                    _fw.freturn(alx::variant(false));
                    return;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    remember(c, "cip.wait " + what + ": still " + text_of(seen) + " after " +
                                    std::to_string(timeout_ms) + " ms");
                    _fw.freturn(alx::variant(false));
                    return;
                }
                const auto next = now + std::chrono::milliseconds(period_ms);
                std::this_thread::sleep_until(next < deadline ? next : deadline);
            }
        }

        // Why the last get/set failed: "" after one that went through.
        void fn_error(fwrap& _fw) {
            cip_conn* c = as_conn(_fw[0], "error");
            std::lock_guard<std::mutex> lk(c->mtx);
            _fw.freturn(alx::variant(c->reason));
        }

        void fn_close(fwrap& _fw) {
            cip_conn* c = as_conn(_fw[0], "close");
            if (c->server) {
                dbg("server close");
                c->srv->close();
                {
                    std::lock_guard<std::mutex> lk(c->peer_mtx);
                    c->peers.clear();
                    c->peer_session.clear();
                }
                dbg("server closed");
                return;
            }
            std::string unregister;
            {
                std::lock_guard<std::mutex> lk(c->mtx);
                if (c->session) { // polite: drop the session before the socket
                    put16(unregister, 0x0066);
                    put16(unregister, 0);
                    put32(unregister, c->session);
                    put32(unregister, 0);
                    put32(unregister, 0); // sender context (8 bytes)
                    put32(unregister, 0);
                    put32(unregister, 0); // options
                    c->session = 0;
                }
            }
            // Sent outside the lock: a synchronous write must not hold the receive path. The frame
            // was built under it, so the session is consistent either way.
            if (!unregister.empty()) c->tr->bytes_send(unregister);
            // Outside the lock: close() joins the receiver thread, which takes this same lock in
            // on_rx -- holding it here would deadlock the join.
            c->tr->close();
        }

        void fn_alive(fwrap& _fw) {
            cip_conn* c = as_conn(_fw[0], "alive");
            if (c->server) {
                _fw.freturn(alx::variant(c->srv && c->srv->is_open()));
                return;
            }
            _fw.freturn(alx::variant(c->tr && c->tr->is_open() && c->tr->connect_num() != 0));
        }

        void fn_open(fwrap& _fw) {
            const std::string& addr = as_str(_fw[0], "cip.open addr");
            int timeout = 3000;
            if (_fw.size() > 1) timeout = static_cast<int>(as_int(_fw[1], "cip.open timeout"));
            std::string why;
            cip_conn* c = open_conn(addr, timeout, why);
            if (!c) fail("open", addr, why);
            _fw.freturn(alx::variant(alx::anyptr_ex<cip_conn>::make_ref(c)));
        }

        // listen(addr): the local point table, served. The define verbs fill it and seed each
        // point's zero; the wire reads and writes it from then on.
        void fn_listen(fwrap& _fw) {
            const std::string& addr_given = as_str(_fw[0], "cip.listen addr");

            std::string addr = addr_given;
            if (addr.find(':') == std::string::npos) addr += ":44818";

            auto* c = new cip_conn();
            c->server = true;
            c->addr = addr;
            alx::transmit* t = alx::transmit::create("tcp_server", addr);
            if (!t) {
                delete c;
                fail("listen", addr_given, "no tcp_server transport");
            }
            t->trans_mesg.connect([c](const alx::uint_64 _loc, bool _connect, const char* _mesg) {
                if (!_connect && _mesg && _loc != 0) { // a peer went away: forget its buffer
                    std::lock_guard<std::mutex> lk(c->peer_mtx);
                    c->peers.erase(_loc);
                    c->peer_session.erase(_loc);
                }
                if (!_connect && _mesg && _loc == 0) {
                    std::lock_guard<std::mutex> lk(c->mtx);
                    c->last_err = _mesg;
                }
            });
            if (!t->open()) { // bind and listen now: a client that connects right after this
                // line must find the socket, and comm's worker would open it later
                const std::string why = c->last_err.empty() ? "bind failed" : c->last_err;
                delete t;
                delete c;
                fail("listen", addr_given, why);
            }
            dbg("listen bound " + addr);
            c->srv = new server_trans(t, c);
            if (!c->srv->open()) {
                delete c;
                fail("listen", addr_given, "the worker would not start");
            }
            _fw.freturn(alx::variant(alx::anyptr_ex<cip_conn>::make_ref(c)));
        }

        bool conn_alive(cip_conn* _c) {
            if (_c->server) return _c->srv && _c->srv->is_open();
            return _c->tr && _c->tr->is_open() && _c->tr->connect_num() != 0;
        }

        std::string conn_desc(const alx::anyptr& _h) {
            cip_conn* c = alx::anyptr_ex<cip_conn>::as(_h);
            if (!c) return std::string();
            return std::string(c->server ? "server " : "client ") + c->addr;
        }

        std::string esc_html(const std::string& _s) {
            std::string out;
            out.reserve(_s.size());
            for (char ch : _s) {
                switch (ch) {
                case '&': out += "&amp;"; break;
                case '<': out += "&lt;"; break;
                case '>': out += "&gt;"; break;
                case '"': out += "&quot;"; break;
                default: out += ch; break;
                }
            }
            return out;
        }

        // The debugger's view: role, liveness, the declared table, and the last reason.
        alx::variant conn_view(const alx::anyptr& _h) {
            cip_conn* c = alx::anyptr_ex<cip_conn>::as(_h);
            if (!c) return alx::variant();
            std::string html = "<h3 style=\"color:#fbf1c7;margin:4px 0\">" +
                               esc_html(std::string(c->server ? "server " : "client ") + c->addr) +
                               "</h3>";
            html += std::string("<p style=\"color:#a89984;margin:4px 0\">alive: ") +
                    (conn_alive(c) ? "true" : "false");
            if (!c->server) html += "<br>session: " + std::to_string(c->session);
            html += "</p>";
            std::unique_lock<std::mutex> lk(c->mtx, std::try_to_lock);
            if (!lk.owns_lock()) {
                html += "<p style=\"color:#fb4934\">points: busy</p>";
            } else {
                html += "<p style=\"color:#a89984;margin:8px 0 4px\">points (" +
                        std::to_string(c->points.size()) + ")</p>";
                if (!c->points.empty()) {
                    html += "<table cellspacing=\"0\" cellpadding=\"3\" width=\"100%\">";
                    html += "<tr><th align=\"left\" style=\"color:#83a598\">name</th>"
                            "<th align=\"left\" style=\"color:#83a598\">type</th></tr>";
                    bool odd = false;
                    for (const auto& kv : c->points) {
                        html += "<tr style=\"background:" + std::string(odd ? "#32302f" : "#282828") +
                                "\"><td><code>" + esc_html(kv.first) + "</code></td><td>" +
                                esc_html(describe(kv.second)) + "</td></tr>";
                        odd = !odd;
                    }
                    html += "</table>";
                }
                if (!c->reason.empty())
                    html += "<p style=\"color:#fb4934\">error: " + esc_html(c->reason) + "</p>";
            }
            alx::varmap m;
            m["kind"] = alx::variant(std::string("html"));
            m["format"] = alx::variant(std::string("html"));
            m["data"] = alx::variant(alx::bytes(html.data(), html.size()));
            return alx::variant(std::move(m));
        }

    } // namespace

    extern "C"
#ifdef _WIN32
        __declspec(dllexport)
#endif
        void alexis_script_load(fwrap& _fw) {
        _fw.bind(alx::anyptr_ex<cip_conn>::rid, "cip::conn", conn_desc, conn_view);
        _fw.bind("open", fn_open);
        _fw.bind("listen", fn_listen);
        _fw.bind("define_bool", fn_define_bool);
        _fw.bind("define_sint", fn_define_sint);
        _fw.bind("define_int", fn_define_int);
        _fw.bind("define_dint", fn_define_dint);
        _fw.bind("define_lint", fn_define_lint);
        _fw.bind("define_real", fn_define_real);
        _fw.bind("define_lreal", fn_define_lreal);
        _fw.bind("define_string", fn_define_string);
        _fw.bind("define_sstring", fn_define_sstring);
        _fw.bind("define_byte", fn_define_byte);
        _fw.bind("remove", fn_remove);
        _fw.bind("points", fn_points);
        _fw.bind("get", fn_get);
        _fw.bind("set", fn_set);
        _fw.bind("wait", fn_wait);
        _fw.bind("error", fn_error);
        _fw.bind("close", fn_close);
        _fw.bind("alive", fn_alive);
    }

} // namespace cip_link
