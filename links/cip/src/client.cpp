// Copyright (c) 2026 AlexisVon
// SPDX-License-Identifier: Apache-2.0

#include "internal.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

/// The client's half: the one-outstanding-request exchange, the session, and the tag verbs the
/// script surface calls (read_tag / write_tag).
namespace cip_link {

    namespace { // the transport's own steps, and the locked lookup

        void on_rx(cip_conn* _c, const alx::bytes_view& _data) {
            std::lock_guard<std::mutex> lk(_c->mtx);
            _c->rx.append(reinterpret_cast<const char*>(_data.data()), _data.size());
            for (;;) {
                if (_c->rx.size() < 24) return;
                const auto* p = reinterpret_cast<const uint8_t*>(_c->rx.data());
                const size_t len = get16(p + 2);
                if (_c->rx.size() < 24 + len) return;
                _c->reply.cmd = get16(p);
                _c->reply.session = get32(p + 4);
                _c->reply.status = get32(p + 8);
                _c->reply.ctx = static_cast<uint64_t>(get32(p + 12)) |
                                (static_cast<uint64_t>(get32(p + 16)) << 32);
                // A command none of ours means the stream lost its framing: every later reply
                // would be garbage, so the handle says so instead of guessing.
                if (get16(p) != 0x0065 && get16(p) != 0x0066 && get16(p) != 0x006F)
                    _c->broken = true;
                _c->reply.payload = _c->rx.substr(24, len);
                _c->rx.erase(0, 24 + len);
                _c->reply_ok = true;
                _c->cv.notify_all();
            }
        }

        // One encapsulated exchange: send before taking the lock, then accept only the reply that
        // echoes this request's context -- a late reply to a timed-out request is never adopted.
        bool exchange(cip_conn* _c, uint16_t _cmd, const std::string& _payload, reply_t& _out,
                      std::string& _why) {
            std::lock_guard<std::mutex> io(_c->io_mtx); // one outstanding request, whichever thread asks
            uint32_t session = 0;
            uint32_t seq = 0;
            {
                std::lock_guard<std::mutex> lk(_c->mtx);
                if (_c->broken) {
                    _why = "the reply stream is out of sync -- close the handle and reopen";
                    return false;
                }
                if (!_c->tr || !_c->tr->is_open() || _c->tr->connect_num() == 0) {
                    _why = "not connected";
                    return false;
                }
                session = _c->session;
                seq = _c->seq++;
            }
            std::string frame;
            put16(frame, _cmd);
            put16(frame, static_cast<uint16_t>(_payload.size()));
            put32(frame, session);
            put32(frame, 0); // status
            put32(frame, seq); // the sender context: this request's identity, echoed back
            put32(frame, 0);
            put32(frame, 0); // options
            frame.append(_payload);

            dbg("client send cmd " + std::to_string(_cmd) + " seq " + std::to_string(seq));
            if (!_c->tr->bytes_send(frame)) {
                _why = "send failed";
                return false;
            }

            std::unique_lock<std::mutex> lk(_c->mtx);
            const uint64_t want = seq; // our context: the sequence, high word zero
            const bool got =
                _c->cv.wait_for(lk, std::chrono::milliseconds(_c->timeout_ms),
                                [&] { return _c->reply_ok && _c->reply.ctx == want; });
            if (_c->broken) {
                _why = "the reply stream went out of sync";
                return false;
            }
            if (!got) {
                _why = "no reply within " + std::to_string(_c->timeout_ms) + " ms";
                return false;
            }
            if (_c->reply.cmd != _cmd) {
                _why = "unexpected reply command";
                return false;
            }
            if (_c->reply.status != 0) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "encapsulation status 0x%08X", _c->reply.status);
                _why = buf;
                return false;
            }
            _out = _c->reply;
            return true;
        }

        // The CIP payload out of a SendRRData reply: interface(4) timeout(2) count(2), then items;
        // 0x00B2 is the unconnected data item (0x0000 the null address).
        bool unwrap_send_data(const std::string& _rep, std::string& _cip, std::string& _why) {
            if (_rep.size() < 8) {
                _why = "short send-rr-data reply";
                return false;
            }
            const auto* p = reinterpret_cast<const uint8_t*>(_rep.data());
            const size_t count = get16(p + 6);
            size_t at = 8;
            for (size_t i = 0; i < count; ++i) {
                if (at + 4 > _rep.size()) break;
                const uint16_t type = get16(p + at);
                const uint16_t len = get16(p + at + 2);
                at += 4;
                if (at + len > _rep.size()) break;
                if (type == 0x00B2) {
                    _cip = _rep.substr(at, len);
                    return true;
                }
                at += len;
            }
            _why = "no unconnected data item in the reply";
            return false;
        }

        // A failed Unconnected Send answers 0xD2 in place of the embedded reply; anything else is
        // the embedded reply itself.
        bool unwrap_unconnected_send(const std::string& _cip, std::string& _out, std::string& _why) {
            if (_cip.empty()) {
                _why = "empty cip reply";
                return false;
            }
            if (static_cast<uint8_t>(_cip[0]) == 0xD2) {
                char buf[64];
                // 0xD2 = service | 0x80, then a reserved byte, then the status (cpppo's own parse)
                const uint8_t st = _cip.size() > 2 ? static_cast<uint8_t>(_cip[2]) : 0;
                std::snprintf(buf, sizeof(buf), "unconnected send refused (status 0x%02X)", st);
                _why = buf;
                return false;
            }
            _out = _cip;
            return true;
        }

        // ---------------------------------------------------------------------------
        // The encapsulated requests: RegisterSession, UnRegisterSession, SendRRData.
        // ---------------------------------------------------------------------------
        bool register_session(cip_conn* _c, std::string& _why) {
            std::string payload;
            put16(payload, 1); // protocol version
            put16(payload, 0); // option flags
            reply_t rep;
            if (!exchange(_c, 0x0065, payload, rep, _why)) return false;
            if (rep.session == 0) {
                _why = "no session handle in the reply";
                return false;
            }
            {
                std::lock_guard<std::mutex> lk(_c->mtx);
                _c->session = rep.session;
            }
            return true;
        }

        // One CIP request in SendRRData + Unconnected Send -- the shape the working client sends,
        // with the route path a real chassis needs (the simulator accepts it too).
        bool send_cip(cip_conn* _c, const std::string& _cip_req, std::string& _cip_rep,
                      std::string& _why) {
            std::string cpf;
            put32(cpf, 0); // interface handle
            put16(cpf, 8); // timeout in seconds; 8 is what the working client sends
            put16(cpf, 2); // item count
            put16(cpf, 0x0000); // null address item
            put16(cpf, 0);
            std::string us;
            put8(us, 0x52); // Unconnected Send
            put_path(us, _c->send);
            put8(us, 5); // priority/time tick: 2^5 == 32 ms, the working client's default
            put8(us, 157); // timeout ticks: 157 * 32 ms ~ 5 s
            put16(us, static_cast<uint16_t>(_cip_req.size()));
            us.append(_cip_req);
            if (_cip_req.size() % 2) put8(us, 0); // pad to a word
            put_route_path(us, _c->route);
            put16(cpf, 0x00B2); // unconnected data item
            put16(cpf, static_cast<uint16_t>(us.size()));
            cpf.append(us);

            reply_t rep;
            if (!exchange(_c, 0x006F, cpf, rep, _why)) return false;
            std::string cip;
            if (!unwrap_send_data(rep.payload, cip, _why)) return false;
            return unwrap_unconnected_send(cip, _cip_rep, _why);
        }

        // The declared point a tag names, by value under the table's own lock: a copy cannot dangle
        // while a request is on the wire, and the caller never touches the map itself.
        bool declared_point(cip_conn* _c, const std::string& _tag, point& _out) {
            tag_ref ref;
            std::string why;
            bool syntax = false;
            std::lock_guard<std::mutex> lk(_c->mtx);
            return lookup_point(_c, _tag, ref, _out, syntax, why);
        }
    } // namespace

    cip_conn* open_conn(const std::string& _addr, int _timeout_ms, std::string& _why) {
        std::string addr = _addr;
        if (addr.find(':') == std::string::npos) addr += ":44818";

        auto* c = new cip_conn();
        c->addr = addr;
        c->timeout_ms = _timeout_ms;
        seg_port_link(c->route, 1, 0); // port 1, link 0: the CPU in the chassis (slot 0)
        seg_class(c->send, 6); // Connection Manager
        seg_instance(c->send, 1);

        c->tr = alx::transmit::create("tcp_client", addr);
        if (!c->tr) {
            _why = "no tcp_client transport";
            delete c;
            return nullptr;
        }
        c->tr->bytes_recv.connect([c](const alx::bytes_view& _data, alx::uint_64) { on_rx(c, _data); });
        c->tr->trans_mesg.connect([c](const alx::uint_64, bool _connect, const char* _mesg) {
            if (!_connect && _mesg) {
                std::lock_guard<std::mutex> lk(c->mtx);
                c->last_err = _mesg;
            }
        });
        if (!c->tr->open()) {
            _why = c->last_err.empty() ? "connect failed" : c->last_err;
            delete c;
            return nullptr;
        }
        dbg("client connected " + addr);
        if (!register_session(c, _why)) {
            delete c;
            return nullptr;
        }
        dbg("client registered");
        return c;
    }

    // Read Tag (0x4C): path, elements; one element gives a scalar, several a vec. _script_err marks
    // what only the script can fix (it raises); a device that says no leaves null plus a reason.
    bool read_tag(cip_conn* _c, const std::string& _tag, alx::variant& _out, bool& _script_err,
                  std::string& _why) {
        _script_err = false;
        tag_ref ref;
        if (!parse_tag_path(_tag, ref, _why)) {
            _script_err = true;
            return false;
        }
        point decl;
        const bool have_decl = declared_point(_c, _tag, decl);
        uint32_t count = ref.count;
        if (have_decl && !fit_count(decl, ref, count, _why)) { // the declared shape, a row, or the range
            _script_err = true;
            return false;
        }
        if (static_cast<size_t>(count) * (have_decl && decl.type ? decl.type->width : 8) >
            kMaxPayloadBytes) {
            _why = "a " + std::to_string(count) + "-element read does not fit one message yet -- "
                                                  "ask for a slice (Tag[a,b])";
            _script_err = true;
            return false;
        }

        std::string path;
        if (!tag_path_segments(ref, path, _why)) {
            _script_err = true;
            return false;
        }

        std::string req;
        put8(req, 0x4C);
        put_path(req, path);
        put16(req, static_cast<uint16_t>(count));

        std::string rep;
        if (!send_cip(_c, req, rep, _why)) return false;
        cip_status st;
        if (!read_status(rep, st, _why)) return false;
        if (st.service != 0xCC) {
            _why = "read tag refused";
            return false;
        }
        if (rep.size() < st.data_at + 2) {
            _why = "short read tag data";
            return false;
        }
        const auto* p = reinterpret_cast<const uint8_t*>(rep.data()) + st.data_at;
        const uint16_t code = get16(p);
        const uint8_t* data = p + 2;
        const cip_type* t = type_by_code(code);
        if (!t) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "unsupported data type 0x%04X", code);
            _why = buf;
            _script_err = true;
            return false;
        }
        { // a declared name must answer with its own type: this is where a wrong point table shows
            if (have_decl && decl.type && decl.type->code != code) {
                _why = "declared " + describe(decl) + ", the device answered " + t->name;
                _script_err = true;
                return false;
            }
        }
        const size_t have = rep.size() - st.data_at - 2;
        if (t->k != cip_type::kind::atomic) {
            if (count != 1) { // the name asked for several elements of a string: never a value
                _why = "a " + std::string(t->name) + " is one value -- no range";
                _script_err = true;
                return false;
            }
            // A string carries its own length: no declared limit on either side.
            const size_t at = t->k == cip_type::kind::str16 ? 2 : 1;
            const size_t len = have >= at ? (t->k == cip_type::kind::str16 ? get16(data) : data[0]) : 0;
            if (have < at + len) {
                _why = "short " + std::string(t->name) + " value";
                return false;
            }
            _out = decode_value(*t, data);
            return true;
        }
        if (have < static_cast<size_t>(count) * t->width) {
            _why = "the device returned " + std::to_string(have / t->width) + " of " +
                   std::to_string(count) + " elements";
            return false;
        }
        if (count == 1) {
            _out = decode_value(*t, data);
            return true;
        }
        if (t->code == 0xD1) { // a byte run comes back as bytes, not a vec of ints
            _out = alx::variant(alx::bytes(data, count * t->width));
            return true;
        }
        alx::varvec out;
        for (uint32_t i = 0; i < count; ++i) out.push_back(decode_value(*t, data + i * t->width));
        if (have_decl && decl.dims[1] != 0 && ref.dims == 0 && count == decl.count)
            _out = nest_2d(decl, std::move(out)); // the whole 2-D point comes back in its shape
        else
            _out = alx::variant(std::move(out));
        return true;
    }

    // Write Tag (0x4D): path, type, elements, data; the type comes from the declaration.
    // _script_err has the meaning read_tag gives it.
    bool write_tag(cip_conn* _c, const std::string& _tag, const alx::variant& _v, const point& _p,
                   bool& _script_err, std::string& _why) {
        _script_err = false;
        const cip_type& type = *_p.type;
        tag_ref ref;
        if (!parse_tag_path(_tag, ref, _why)) {
            _script_err = true;
            return false;
        }
        uint32_t count = ref.count;
        if (!fit_count(_p, ref, count, _why)) { // the declared shape, a row, or the range
            _script_err = true;
            return false;
        }
        if (static_cast<size_t>(count) * type.width > kMaxPayloadBytes) {
            _why = "a " + std::to_string(count) + "-element write does not fit one message yet";
            _script_err = true;
            return false;
        }

        std::string data;
        if (type.k != cip_type::kind::atomic) {
            if (!_v.is<std::string>()) {
                _why = "a " + std::string(type.name) + " point takes a string";
                _script_err = true;
                return false;
            }
            if (!pack_value(type, _v, data, _why)) {
                _script_err = true;
                return false;
            }
        } else if (by_byte(_p)) {
            if (!byte_run(_p, count, _v, data, _why)) {
                _script_err = true;
                return false;
            }
        } else if (count == 1) {
            if (!pack_value(type, _v, data, _why)) {
                _script_err = true;
                return false;
            }
        } else {
            if (!_v.is<alx::varvec>()) {
                _why = "a " + std::to_string(count) + "-element point takes a vec";
                _script_err = true;
                return false;
            }
            const alx::varvec* vs = &_v.to<alx::varvec>();
            alx::varvec flat;
            if (ref.dims == 0 && _p.dims[1] != 0) { // the whole 2-D point: nested or flat
                if (!flatten_2d(_p, *vs, flat, _why)) {
                    _script_err = true;
                    return false;
                }
                vs = &flat;
            }
            if (vs->size() != count) {
                _why = "the point holds " + std::to_string(count) + " elements, the vec has " +
                       std::to_string(vs->size());
                _script_err = true;
                return false;
            }
            for (const alx::variant& e : *vs)
                if (!pack_value(type, e, data, _why)) {
                    _script_err = true;
                    return false;
                }
        }

        std::string path;
        if (!tag_path_segments(ref, path, _why)) {
            _script_err = true;
            return false;
        }
        std::string req;
        put8(req, 0x4D);
        put_path(req, path);
        put16(req, type.code);
        put16(req, static_cast<uint16_t>(count));
        req.append(data);

        std::string rep;
        if (!send_cip(_c, req, rep, _why)) return false;
        cip_status st;
        if (!read_status(rep, st, _why)) return false;
        if (st.service != 0xCD) {
            _why = "write tag refused";
            return false;
        }
        return true;
    }
} // namespace cip_link
