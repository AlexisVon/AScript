// Copyright (c) 2026 AlexisVon
// SPDX-License-Identifier: Apache-2.0

#include "internal.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

/// The server side: the same codec mirrored. The local point table is the script's own state --
/// the wire reads and writes it, and get/set on a server area touch it directly.
namespace cip_link {

    namespace { // the request path: parse a frame, serve it, wrap the reply

        alx::variant zero_element(const cip_type& _t) {
            switch (_t.code) {
            case 0xC1: return alx::variant(false);
            case 0xCA:
            case 0xCB: return alx::variant(0.0);
            default: return alx::variant(static_cast<alx::int_64>(0));
            }
        }

        // The 24-byte header: the sender context comes back byte for byte.
        void encap(std::string& _out, uint16_t _cmd, uint32_t _session, const std::string& _ctx,
                   const std::string& _payload) {
            put16(_out, _cmd);
            put16(_out, static_cast<uint16_t>(_payload.size()));
            put32(_out, _session);
            put32(_out, 0); // status
            _out.append(_ctx);
            put32(_out, 0); // options
            _out.append(_payload);
        }

        // SendRRData's wrapper: interface, timeout, two items -- null address, then data.
        std::string cpf_wrap(const std::string& _payload) {
            std::string out;
            put32(out, 0);
            put16(out, 8);
            put16(out, 2);
            put16(out, 0x0000);
            put16(out, 0);
            put16(out, 0x00B2);
            put16(out, static_cast<uint16_t>(_payload.size()));
            out.append(_payload);
            return out;
        }

        // An EPATH back to a name and its trailing element indices (0x28/0x29 one, 0x2A/0x2B a
        // pair); a symbol after an element is refused -- the indices go last.
        bool path_to_point(const std::string& _path, tag_ref& _out, std::string& _why) {
            _out = tag_ref();
            size_t at = 0;
            bool elements = false;
            while (at < _path.size()) {
                const auto seg = static_cast<uint8_t>(_path[at]);
                if (seg == 0x00 && at + 1 == _path.size()) break; // the odd path's word pad
                if (seg == 0x91) {
                    if (elements) {
                        _why = "a symbol after an element segment -- the indices go last";
                        return false;
                    }
                    if (at + 2 > _path.size()) {
                        _why = "truncated symbol segment";
                        return false;
                    }
                    const size_t len = static_cast<uint8_t>(_path[at + 1]);
                    if (at + 2 + len > _path.size()) {
                        _why = "truncated symbol segment";
                        return false;
                    }
                    if (!_out.name.empty()) _out.name += '.';
                    _out.name.append(_path, at + 2, len);
                    at += 2 + len + (len % 2); // the pad belongs to the segment
                    continue;
                }
                uint32_t got[2] = {0, 0};
                int n = 0;
                if (seg == 0x28 || seg == 0x29) {
                    const size_t width = seg == 0x28 ? 1 : 2;
                    if (at + 1 + width > _path.size()) {
                        _why = "truncated element segment";
                        return false;
                    }
                    got[0] = seg == 0x28
                                 ? static_cast<uint8_t>(_path[at + 1])
                                 : get16(reinterpret_cast<const uint8_t*>(_path.data()) + at + 1);
                    n = 1;
                    at += 1 + width;
                } else if (seg == 0x2A || seg == 0x2B) { // the index pair: two dimensions at once
                    const size_t width = seg == 0x2A ? 1 : 2;
                    if (at + 1 + 2 * width > _path.size()) {
                        _why = "truncated element pair segment";
                        return false;
                    }
                    const auto* p = reinterpret_cast<const uint8_t*>(_path.data()) + at + 1;
                    got[0] = width == 1 ? p[0] : get16(p);
                    got[1] = width == 1 ? p[1] : get16(p + 2);
                    n = 2;
                    at += 1 + 2 * width;
                } else {
                    char buf[80];
                    std::snprintf(buf, sizeof(buf),
                                  "path segment 0x%02X is not a name or an element", seg);
                    _why = buf;
                    return false;
                }
                if (_out.dims + n > 2) {
                    _why = "more than two indices in the path -- this table carries two";
                    return false;
                }
                for (int i = 0; i < n; ++i) _out.index[_out.dims + i] = got[i];
                _out.dims += n;
                elements = true;
            }
            if (_out.name.empty()) {
                _why = "empty path";
                return false;
            }
            return true;
        }

        // The element _i of a point's local value; false when the point does not hold that many.
        bool local_element(const point& _p, const alx::variant& _local, uint32_t _i,
                           alx::variant& _out) {
            if (_p.type->k != cip_type::kind::atomic) {
                if (_i != 0) return false;
                _out = _local;
                return true;
            }
            if (by_byte(_p)) {
                if (!_local.is<alx::bytes>()) return false;
                const alx::bytes& b = _local.to<alx::bytes>();
                if (_i >= b.size()) return false;
                _out = alx::variant(static_cast<alx::int_64>(b.cdata()[_i]));
                return true;
            }
            if (_p.count == 1) {
                if (_i != 0) return false;
                _out = _local;
                return true;
            }
            if (!_local.is<alx::varvec>()) return false;
            const alx::varvec& v = _local.to<alx::varvec>();
            if (_i >= v.size()) return false;
            _out = v[_i];
            return true;
        }

        // The element _i as a slot a write can land in: the write side of local_element.
        alx::variant* local_slot(const point& _p, alx::variant& _local, uint32_t _i) {
            if (_p.type->k != cip_type::kind::atomic || by_byte(_p)) return nullptr; // written in place
            if (_p.count == 1) return _i == 0 ? &_local : nullptr;
            if (!_local.is<alx::varvec>()) return nullptr;
            alx::varvec& v = _local.as<alx::varvec>();
            return _i < v.size() ? &v[_i] : nullptr;
        }

        std::string serve_multi(cip_conn* _c, const std::string& _data);

        // One request in, one reply out. Status codes are CIP's: 0x05 no such path, 0x03 value
        // refused, 0x08 unsupported service, 0x13 short data.
        std::string serve_cip(cip_conn* _c, const std::string& _req) {
            if (_req.size() < 2) return cip_reply(0, 0x13, std::string());
            const auto service = static_cast<uint8_t>(_req[0]);
            const size_t path_size = static_cast<uint8_t>(_req[1]);
            if (_req.size() < 2 + 2 * path_size) return cip_reply(service, 0x13, std::string());
            const std::string path = _req.substr(2, 2 * path_size);
            const std::string data = _req.substr(2 + 2 * path_size);
            if (service == 0x0A) return serve_multi(_c, data); // Multiple Service Packet: a batch

            std::string why;
            tag_ref ref;
            if (!path_to_point(path, ref, why)) return cip_reply(service, 0x05, std::string());

            std::lock_guard<std::mutex> lk(_c->mtx);
            auto it = _c->points.find(ref.name);
            if (it == _c->points.end()) return cip_reply(service, 0x05, std::string());
            const point& p = it->second;
            uint32_t index = 0;
            if (!linear_start(p, ref, index, why)) return cip_reply(service, 0x05, std::string());
            alx::variant& local = _c->locals[ref.name];

            if (service == 0x4C) { // Read Tag
                uint16_t elements = 1;
                if (data.size() >= 2) elements = get16(reinterpret_cast<const uint8_t*>(data.data()));
                if (elements == 0) elements = 1;
                if (elements > p.count - index) return cip_reply(service, 0x03, std::string());
                std::string body;
                put16(body, p.type->code);
                for (uint16_t i = 0; i < elements; ++i) {
                    alx::variant e;
                    if (!local_element(p, local, index + i, e))
                        return cip_reply(service, 0x03, std::string());
                    if (!pack_value(*p.type, e, body, why))
                        return cip_reply(service, 0x03, std::string());
                    // The length fields are 16-bit: a body that will not fit one message is
                    // refused (0x03) instead of wrapping a length and desyncing the peer's stream.
                    if (body.size() > kMaxPayloadBytes) return cip_reply(service, 0x03, std::string());
                }
                return cip_reply(service, 0x00, body);
            }

            if (service == 0x4D) { // Write Tag
                if (data.size() < 4) return cip_reply(service, 0x13, std::string());
                const auto* d = reinterpret_cast<const uint8_t*>(data.data());
                const uint16_t type = get16(d);
                const uint16_t elements = get16(d + 2);
                if (elements == 0) return cip_reply(service, 0x13, std::string()); // wrote nothing
                if (type != p.type->code) return cip_reply(service, 0x03, std::string());
                if (elements > p.count - index) return cip_reply(service, 0x03, std::string());
                std::vector<alx::variant> vals;
                size_t at = 4;
                for (uint16_t i = 0; i < elements; ++i) {
                    size_t need = p.type->width;
                    if (p.type->k != cip_type::kind::atomic) {
                        if (data.size() < at + 1) return cip_reply(service, 0x13, std::string());
                        const size_t len = p.type->k == cip_type::kind::str16 ? get16(d + at) : d[at];
                        need = (p.type->k == cip_type::kind::str16 ? 2 : 1) + len;
                        if (p.type->k == cip_type::kind::str16 && len % 2) need += 1; // the odd pad
                    }
                    if (data.size() < at + need) return cip_reply(service, 0x13, std::string());
                    vals.push_back(decode_value(*p.type, d + at));
                    at += need;
                }

                if (p.type->k != cip_type::kind::atomic) {
                    if (index != 0 || elements != 1) return cip_reply(service, 0x03, std::string());
                    local = vals[0];
                } else if (by_byte(p)) {
                    if (!local.is<alx::bytes>()) return cip_reply(service, 0x03, std::string());
                    alx::bytes& b = local.as<alx::bytes>();
                    if (index + elements > b.size()) return cip_reply(service, 0x03, std::string());
                    for (uint16_t i = 0; i < elements; ++i) {
                        b.data()[index + i] = static_cast<alx::uint_8>(vals[i].to<alx::int_64>());
                    }
                } else if (p.count == 1) {
                    if (index != 0 || elements != 1) return cip_reply(service, 0x03, std::string());
                    local = vals[0];
                } else {
                    if (!local.is<alx::varvec>()) return cip_reply(service, 0x03, std::string());
                    alx::varvec& v = local.as<alx::varvec>();
                    if (index + elements > v.size()) return cip_reply(service, 0x03, std::string());
                    for (uint16_t i = 0; i < elements; ++i) v[index + i] = vals[i];
                }
                // A remote write is what a waiting script is waiting for.
                _c->cv.notify_all();
                return cip_reply(service, 0x00, std::string());
            }

            return cip_reply(service, 0x08, std::string());
        }

        // Multiple Service Packet: serve each sub-request whole, lay the replies behind the count
        // word and its offsets; the envelope says 0x1E when an embedded service said no.
        std::string serve_multi(cip_conn* _c, const std::string& _data) {
            const auto* p = reinterpret_cast<const uint8_t*>(_data.data());
            if (_data.size() < 2) return cip_reply(0x0A, 0x13, std::string());
            const size_t count = get16(p);
            if (count == 0 || _data.size() < 2 + 2 * count) return cip_reply(0x0A, 0x13, std::string());

            std::vector<std::string> replies;
            uint8_t status = 0x00;
            size_t total = 2 + 2 * count;
            for (size_t i = 0; i < count; ++i) {
                const size_t begin = get16(p + 2 + 2 * i);
                const size_t end = i + 1 < count ? get16(p + 2 + 2 * (i + 1)) : _data.size();
                if (begin > end || end > _data.size()) return cip_reply(0x0A, 0x13, std::string());
                replies.push_back(serve_cip(_c, _data.substr(begin, end - begin)));
                if (static_cast<uint8_t>(replies.back()[2]) != 0) status = 0x1E;
                total += replies.back().size();
                if (total > kMaxPayloadBytes) return cip_reply(0x0A, 0x03, std::string());
            }

            std::string body;
            put16(body, static_cast<uint16_t>(count));
            size_t offset = 2 + 2 * count;
            for (const std::string& r : replies) {
                put16(body, static_cast<uint16_t>(offset));
                offset += r.size();
            }
            for (const std::string& r : replies) body.append(r);
            return cip_reply(0x0A, status, body);
        }

        // The Unconnected Send wrapper, stripped when present: 0x52, the send path, priority and
        // timeout ticks, the embedded length, the message, a pad when its length is odd, the route.
        bool strip_unconnected(const std::string& _cip, std::string& _out, std::string& _why) {
            if (_cip.empty()) {
                _why = "empty cip request";
                return false;
            }
            if (static_cast<uint8_t>(_cip[0]) != 0x52) {
                _out = _cip;
                return true;
            }
            size_t at = 1;
            if (_cip.size() < at + 1) {
                _why = "short unconnected send";
                return false;
            }
            at += 1 + 2 * static_cast<size_t>(static_cast<uint8_t>(_cip[at])); // path
            at += 2; // priority, timeout ticks
            if (_cip.size() < at + 2) {
                _why = "short unconnected send";
                return false;
            }
            const size_t len = get16(reinterpret_cast<const uint8_t*>(_cip.data()) + at);
            at += 2;
            if (_cip.size() < at + len) {
                _why = "truncated unconnected send";
                return false;
            }
            _out = _cip.substr(at, len);
            return true;
        }

        // One request frame in, one reply frame out; false when the frame needs no reply.
        bool server_frame(cip_conn* _c, alx::uint_64 _loc, uint16_t _cmd, uint32_t _session,
                          const std::string& _ctx, const std::string& _payload, std::string& _frame) {
            if (_cmd == 0x0065) { // RegisterSession: hand out a session and echo the payload
                uint32_t session = 0;
                {
                    std::lock_guard<std::mutex> lk(_c->peer_mtx);
                    session = _c->peer_session[_loc] = _c->next_session++;
                }
                encap(_frame, _cmd, session, _ctx, _payload);
                return true;
            }
            if (_cmd == 0x0066) { // UnRegisterSession: no reply
                std::lock_guard<std::mutex> lk(_c->peer_mtx);
                _c->peer_session.erase(_loc);
                return false;
            }
            if (_cmd != 0x006F) return false; // anything else has no answer here

            // SendRRData: scan the CPF items for the unconnected data item.
            std::string cip;
            const auto* p = reinterpret_cast<const uint8_t*>(_payload.data());
            if (_payload.size() >= 8) {
                const size_t count = get16(p + 6);
                size_t at = 8;
                for (size_t i = 0; i < count && at + 4 <= _payload.size(); ++i) {
                    const uint16_t type = get16(p + at);
                    const uint16_t len = get16(p + at + 2);
                    at += 4;
                    if (at + len > _payload.size()) break;
                    if (type == 0x00B2) {
                        cip = _payload.substr(at, len);
                        break;
                    }
                    at += len;
                }
            }
            if (cip.empty()) return false;

            std::string req, why;
            if (!strip_unconnected(cip, req, why)) {
                encap(_frame, _cmd, _session, _ctx, cpf_wrap(cip_reply(0, 0x13, std::string())));
                return true;
            }
            encap(_frame, _cmd, _session, _ctx, cpf_wrap(serve_cip(_c, req)));
            return true;
        }
    } // namespace

    // The transport thread's door: accumulate per peer, hand each complete frame to
    // server_frame, send the reply back to that peer.
    void server_trans::on_bytes_recv(const alx::bytes_view& _data, alx::uint_64 _loc) {
        if (!c) return;
        dbg("server got " + std::to_string(_data.size()) + " bytes from " + std::to_string(_loc));
        {
            std::lock_guard<std::mutex> lk(c->peer_mtx);
            c->peers[_loc].append(reinterpret_cast<const char*>(_data.data()), _data.size());
        }
        for (;;) {
            std::string frame, ctx;
            uint16_t cmd = 0;
            uint32_t session = 0;
            {
                // Fold one complete frame out under the lock; serve it outside, because
                // serving may take peer_mtx itself (the session table).
                std::lock_guard<std::mutex> lk(c->peer_mtx);
                std::string& rx = c->peers[_loc];
                if (rx.size() < 24) return;
                const auto* p = reinterpret_cast<const uint8_t*>(rx.data());
                const size_t len = get16(p + 2);
                if (rx.size() < 24 + len) return;
                cmd = get16(p);
                session = get32(p + 4);
                ctx.assign(rx, 12, 8);
                frame = rx.substr(24, len);
                rx.erase(0, 24 + len);
            }
            std::string reply;
            if (server_frame(c, _loc, cmd, session, ctx, frame, reply)) {
                dbg("server replies cmd " + std::to_string(cmd) + " with " +
                    std::to_string(reply.size()) + " bytes");
                c->srv->bytes_send(alx::bytes_view(alx::bytes(reply), 0, reply.size()), _loc);
            }
        }
    }

    // What a declared point starts at: zeros, false, empty -- a device's own power-on state.
    alx::variant zero_of(const point& _p) {
        if (_p.type->k != cip_type::kind::atomic) return alx::variant(std::string());
        if (by_byte(_p))
            return alx::variant(alx::bytes(static_cast<alx::uint_64>(_p.count), 0));
        if (_p.count > 1) {
            alx::varvec v;
            for (uint32_t i = 0; i < _p.count; ++i) v.push_back(zero_element(*_p.type));
            return alx::variant(std::move(v));
        }
        return zero_element(*_p.type);
    }

    // The server's local read, straight off the table: the whole shape, a row, or a run. A missing
    // name or a run past the end is an empty read with a reason; only a bad tag is a script error.
    bool local_read_locked(cip_conn* _c, const std::string& _tag, alx::variant& _out,
                           bool& _script_err, std::string& _why) {
        _script_err = false;
        tag_ref ref;
        point p;
        if (!lookup_point(_c, _tag, ref, p, _script_err, _why)) {
            if (!_script_err)
                _why = "not declared on this server -- a define line for it comes first";
            return false;
        }
        uint32_t start = 0, count = 1;
        if (!linear_from(p, ref, start, count, _why)) return false;
        const alx::variant& local = _c->locals[ref.name];
        alx::variant first;
        if (!local_element(p, local, start, first)) {
            _why = "element " + std::to_string(start) + " is outside " + describe(p);
            return false;
        }
        if (count == 1) {
            _out = first;
            return true;
        }
        if (by_byte(p)) {
            alx::bytes b(static_cast<alx::uint_64>(count));
            for (uint32_t i = 0; i < count; ++i) {
                alx::variant e;
                if (!local_element(p, local, start + i, e)) {
                    _why = "element " + std::to_string(start + i) + " is outside " + describe(p);
                    return false;
                }
                b.data()[i] = static_cast<alx::uint_8>(e.to<alx::int_64>());
            }
            _out = alx::variant(std::move(b));
            return true;
        }
        alx::varvec v;
        for (uint32_t i = 0; i < count; ++i) {
            alx::variant e;
            if (!local_element(p, local, start + i, e)) {
                _why = "element " + std::to_string(start + i) + " is outside " + describe(p);
                return false;
            }
            v.push_back(std::move(e));
        }
        if (p.dims[1] != 0 && ref.dims == 0)
            _out = nest_2d(p, std::move(v)); // the whole 2-D point comes back in its shape
        else
            _out = alx::variant(std::move(v));
        return true;
    }

    bool local_read(cip_conn* _c, const std::string& _tag, alx::variant& _out, bool& _script_err,
                    std::string& _why) {
        std::lock_guard<std::mutex> lk(_c->mtx);
        return local_read_locked(_c, _tag, _out, _script_err, _why);
    }

    // The server's local write, with the checks the wire applies. A missing name or a run past the
    // end leaves false and a reason; only a bad tag or a wrong value kind is a script error.
    bool local_write_locked(cip_conn* _c, const std::string& _tag, const alx::variant& _v,
                            bool& _script_err, std::string& _why) {
        _script_err = false;
        tag_ref ref;
        point p;
        if (!lookup_point(_c, _tag, ref, p, _script_err, _why)) {
            if (!_script_err)
                _why = "not declared on this server -- a define line for it comes first";
            return false;
        }
        uint32_t start = 0, span = 1;
        if (!linear_from(p, ref, start, span, _why)) return false;
        alx::variant& local = _c->locals[ref.name];

        if (p.type->k != cip_type::kind::atomic) { // a string
            std::string scratch;
            if (!pack_value(*p.type, _v, scratch, _why)) { // the kind, and the wire's own ceiling
                _script_err = true;
                return false;
            }
            local = _v;
            _c->cv.notify_all();
            return true;
        }
        if (by_byte(p)) {
            std::string buf;
            if (!byte_run(p, span, _v, buf, _why)) {
                _script_err = true;
                return false;
            }
            alx::bytes& cur = local.as<alx::bytes>();
            for (size_t i = 0; i < buf.size(); ++i)
                cur.data()[start + i] = static_cast<alx::uint_8>(buf[i]);
            _c->cv.notify_all();
            return true;
        }
        if (p.count == 1) { // a scalar point
            std::string scratch;
            if (!pack_value(*p.type, _v, scratch, _why)) { // the kind check, packing aside
                _script_err = true;
                return false;
            }
            local = _v;
            _c->cv.notify_all();
            return true;
        }
        if (_v.is<alx::varvec>()) { // a run of elements -- the whole point takes it too
            const alx::varvec* vs = &_v.to<alx::varvec>();
            alx::varvec flat;
            if (ref.dims == 0 && p.dims[1] != 0) { // the whole 2-D point: nested or flat
                if (!flatten_2d(p, *vs, flat, _why)) {
                    _script_err = true;
                    return false;
                }
                vs = &flat;
            }
            if (vs->size() != span) {
                _why = "the run holds " + std::to_string(span) + " elements, the vec has " +
                       std::to_string(vs->size());
                _script_err = true;
                return false;
            }
            alx::varvec& cur = local.as<alx::varvec>();
            for (size_t i = 0; i < vs->size(); ++i) {
                std::string scratch;
                if (!pack_value(*p.type, (*vs)[i], scratch, _why)) {
                    _script_err = true;
                    return false;
                }
                cur[start + i] = (*vs)[i];
            }
            _c->cv.notify_all();
            return true;
        }
        if (span != 1) { // one scalar, but the tag names a run
            _why = "declared " + describe(p) + " -- a vec of " + std::to_string(span) +
                   " writes that run";
            _script_err = true;
            return false;
        }
        std::string scratch;
        if (!pack_value(*p.type, _v, scratch, _why)) { // one element of an array, a scalar
            _script_err = true;
            return false;
        }
        alx::variant* slot = local_slot(p, local, start);
        if (!slot) {
            _why = describe(p) + " has no element " + std::to_string(start);
            _script_err = true;
            return false;
        }
        *slot = _v;
        _c->cv.notify_all();
        return true;
    }

    bool local_write(cip_conn* _c, const std::string& _tag, const alx::variant& _v,
                     bool& _script_err, std::string& _why) {
        std::lock_guard<std::mutex> lk(_c->mtx);
        return local_write_locked(_c, _tag, _v, _script_err, _why);
    }
} // namespace cip_link
