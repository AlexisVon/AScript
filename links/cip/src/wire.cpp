// Copyright (c) 2026 AlexisVon
// SPDX-License-Identifier: Apache-2.0

#include "internal.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/// The wire: what the bytes mean -- the data-type table, EPATH segments and paths, the tag
/// parser, the value codec, the reply header, and a declared point's geometry.
namespace cip_link {

    namespace { // the segments and helpers only this file's parsers use

        void put_float(std::string& _b, float _v) {
            uint32_t bits = 0;
            std::memcpy(&bits, &_v, sizeof(bits));
            put32(_b, bits);
        }

        float get_float(const uint8_t* _p) {
            const uint32_t bits = get32(_p);
            float v = 0;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }

        void seg_symbolic(std::string& _p, const std::string& _name) {
            put8(_p, 0x91); // ANSI extended symbol
            put8(_p, static_cast<uint8_t>(_name.size()));
            _p.append(_name);
            if (_name.size() % 2) put8(_p, 0); // pad to a word
        }

        // An element segment is two bytes for an index within a byte -- the only form this
        // client sends; the 16-bit form's padding has not been checked against a device.
        bool seg_element(std::string& _p, uint32_t _index, std::string& _why) {
            if (_index > 0xFF) {
                _why = "an element index above 255 is not spelled yet";
                return false;
            }
            put8(_p, 0x28);
            put8(_p, static_cast<uint8_t>(_index));
            return true;
        }

        // The 2-D index pair (0x2A + two 8-bit indices): the pair is how the spec spells a 2-D
        // element; two separate 0x28 segments are not.
        void seg_element_pair(std::string& _p, uint32_t _i, uint32_t _j) {
            put8(_p, 0x2A);
            put8(_p, static_cast<uint8_t>(_i));
            put8(_p, static_cast<uint8_t>(_j));
        }

        bool parse_index(const std::string& _group, const std::string& _tag, uint32_t& _from,
                         uint32_t& _count, std::string& _why) {
            const size_t comma = _group.find(',');
            const std::string first = comma == std::string::npos ? _group : _group.substr(0, comma);
            const std::string last = comma == std::string::npos ? _group : _group.substr(comma + 1);
            for (const std::string& side : {first, last}) {
                if (side.empty() || side.size() > 9 ||
                    side.find_first_not_of("0123456789") != std::string::npos) {
                    _why = "element '" + _group + "' in '" + _tag + "' is not an index or a range";
                    return false;
                }
            }
            const uint32_t from = static_cast<uint32_t>(std::stoul(first));
            const uint32_t to = static_cast<uint32_t>(std::stoul(last));
            if (comma == std::string::npos) { // one element
                _from = from;
                _count = 1;
                return true;
            }
            if (to <= from) {
                _why = "range '" + _group + "' in '" + _tag + "' is empty ([a,b) holds a..b-1)";
                return false;
            }
            _from = from;
            _count = to - from;
            return true;
        }
    } // namespace

    // ASCRIPT_CIP_DEBUG=1: what the wire did, on stderr. A developer switch, off in every
    // normal run.
    void dbg(const std::string& _what) {
        static const bool on = std::getenv("ASCRIPT_CIP_DEBUG") != nullptr;
        if (on) std::fprintf(stderr, "cip: %s\n", _what.c_str());
    }

    // ---------------------------------------------------------------------------
    // Bytes, little-endian on the wire.
    // ---------------------------------------------------------------------------
    void put8(std::string& _b, uint8_t _v) {
        _b.push_back(static_cast<char>(_v));
    }

    void put16(std::string& _b, uint16_t _v) {
        _b.push_back(static_cast<char>(_v & 0xFF));
        _b.push_back(static_cast<char>(_v >> 8));
    }

    void put32(std::string& _b, uint32_t _v) {
        put16(_b, static_cast<uint16_t>(_v & 0xFFFF));
        put16(_b, static_cast<uint16_t>(_v >> 16));
    }

    uint16_t get16(const uint8_t* _p) {
        return static_cast<uint16_t>(_p[0] | (_p[1] << 8));
    }

    uint32_t get32(const uint8_t* _p) {
        return static_cast<uint32_t>(get16(_p)) | (static_cast<uint32_t>(get16(_p + 2)) << 16);
    }

    // ---------------------------------------------------------------------------
    // CIP data types: the atomic set a Logix tag space uses, with their wire codes.
    // ---------------------------------------------------------------------------

    const cip_type kTypes[] = {
        {"BOOL", 0xC1, 1, cip_type::kind::atomic},
        {"SINT", 0xC2, 1, cip_type::kind::atomic},
        {"INT", 0xC3, 2, cip_type::kind::atomic},
        {"DINT", 0xC4, 4, cip_type::kind::atomic},
        {"LINT", 0xC5, 8, cip_type::kind::atomic},
        {"REAL", 0xCA, 4, cip_type::kind::atomic},
        {"LREAL", 0xCB, 8, cip_type::kind::atomic},
        {"STRING", 0xD0, 1, cip_type::kind::str16},
        {"BYTE", 0xD1, 1, cip_type::kind::atomic},
        {"SSTRING", 0xDA, 1, cip_type::kind::str8},
    };

    const cip_type* named(const char* _name) {
        for (const cip_type& t : kTypes)
            if (std::strcmp(t.name, _name) == 0) return &t;
        return nullptr;
    }

    const cip_type* type_by_code(uint16_t _code) {
        for (const cip_type& t : kTypes)
            if (t.code == _code) return &t;
        return nullptr;
    }

    void seg_class(std::string& _p, uint32_t _id) {
        if (_id <= 0xFF) {
            put8(_p, 0x20);
            put8(_p, static_cast<uint8_t>(_id));
        } else {
            put8(_p, 0x21);
            put16(_p, static_cast<uint16_t>(_id));
        }
    }

    void seg_instance(std::string& _p, uint32_t _id) {
        if (_id <= 0xFF) {
            put8(_p, 0x24);
            put8(_p, static_cast<uint8_t>(_id));
        } else {
            put8(_p, 0x25);
            put16(_p, static_cast<uint16_t>(_id));
        }
    }

    void seg_port_link(std::string& _p, uint8_t _port, uint8_t _link) {
        put8(_p, _port); // port 01-0E, numeric link address follows
        put8(_p, _link);
    }

    // A path: size in words, then the segments.
    void put_path(std::string& _b, const std::string& _segments) {
        put8(_b, static_cast<uint8_t>(_segments.size() / 2));
        _b.append(_segments);
    }

    // A route path: size in words, one pad byte, then the segments.
    void put_route_path(std::string& _b, const std::string& _segments) {
        put8(_b, static_cast<uint8_t>(_segments.size() / 2));
        put8(_b, 0); // pad
        _b.append(_segments);
    }

    bool parse_tag_path(const std::string& _tag, tag_ref& _out, std::string& _why) {
        _out = tag_ref();
        uint32_t from[2] = {0, 0}; // per group, collected last bracket first
        uint32_t len[2] = {1, 1};
        bool ranged[2] = {false, false};
        int groups = 0;
        size_t at = _tag.size();
        while (at > 0 && _tag[at - 1] == ']') {
            if (groups == 2) {
                _why = "'" + _tag + "' carries more than two indices -- this build spells two";
                return false;
            }
            const size_t open = _tag.rfind('[', at - 1);
            if (open == std::string::npos) {
                _why = "'" + _tag + "' closes a ']' that was never opened";
                return false;
            }
            const std::string group = _tag.substr(open + 1, at - open - 2);
            if (!parse_index(group, _tag, from[groups], len[groups], _why)) return false;
            ranged[groups] = group.find(',') != std::string::npos;
            ++groups;
            at = open;
        }
        _out.name = _tag.substr(0, at);
        if (_out.name.empty()) {
            _why = "'" + _tag + "' has no name before its index";
            return false;
        }
        if (groups == 2 && ranged[1]) {
            _why = "a range is only the last index in '" + _tag + "' -- an earlier one would be "
                                                                  "a strided read, which this build does not spell";
            return false;
        }
        if (groups == 2) {
            _out.index[0] = from[1];
            _out.index[1] = from[0];
        } else if (groups == 1) {
            _out.index[0] = from[0];
        }
        _out.dims = groups;
        if (groups > 0) _out.count = len[0];
        return true;
    }

    // The EPATH a client request carries for a tag: symbolic segments for the name (a dot
    // splits them), then the trailing indices as element segments.
    bool tag_path_segments(const tag_ref& _ref, std::string& _segments, std::string& _why) {
        size_t at = 0;
        for (;;) {
            const size_t dot = _ref.name.find('.', at);
            const std::string part =
                _ref.name.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
            if (part.empty()) {
                _why = "empty name segment in '" + _ref.name + "'";
                return false;
            }
            if (part.size() > 255) { // the segment's length is one byte: refuse, never truncate
                _why = "a name segment longer than 255 characters in '" + _ref.name + "'";
                return false;
            }
            seg_symbolic(_segments, part);
            if (dot == std::string::npos) break;
            at = dot + 1;
        }
        if (_ref.dims == 1) return seg_element(_segments, _ref.index[0], _why);
        if (_ref.dims == 2) {
            if (_ref.index[0] > 0xFF || _ref.index[1] > 0xFF) {
                _why = "a 2-D index above 255 is not spelled yet";
                return false;
            }
            seg_element_pair(_segments, _ref.index[0], _ref.index[1]);
            // The pair is three bytes: the path is padded to a word, and the size field is in
            // words. The pad goes last because an element segment is always last here.
            if (_segments.size() % 2) put8(_segments, 0);
        }
        return true;
    }

    // A BYTE point: one wire element is one byte, and its value rides as bytes.
    bool by_byte(const point& _p) {
        return _p.type && _p.type->code == 0xD1;
    }

    std::string describe(const point& _p) {
        std::string s = _p.type ? _p.type->name : "?";
        if (_p.dims[1] != 0)
            return s + "[" + std::to_string(_p.dims[0]) + "][" + std::to_string(_p.dims[1]) + "]";
        if (_p.count != 1) s += "[" + std::to_string(_p.count) + "]";
        return s;
    }

    bool read_status(const std::string& _cip, cip_status& _st, std::string& _why) {
        if (_cip.size() < 4) {
            _why = "short cip reply";
            return false;
        }
        const auto* p = reinterpret_cast<const uint8_t*>(_cip.data());
        _st.service = p[0];
        const uint8_t status = p[2];
        const size_t ext_words = p[3];
        _st.data_at = 4 + 2 * ext_words;
        if (_cip.size() < _st.data_at) {
            _why = "truncated extended status";
            return false;
        }
        if (status != 0) {
            // A Logix device puts the real reason in the extended status word (the general
            // status is often just 0xFF): carry the first word along when there is one.
            char buf[96];
            if (ext_words > 0 && _cip.size() >= _st.data_at)
                std::snprintf(buf, sizeof(buf), "status 0x%02X (ext 0x%04X)", status, get16(p + 4));
            else
                std::snprintf(buf, sizeof(buf), "status 0x%02X", status);
            _why = buf;
            return false;
        }
        return true;
    }

    // A CIP reply in the flavor this client (and the simulator) speak: service | 0x80, a
    // reserved byte, the general status, the extended-status size in words.
    std::string cip_reply(uint8_t _service, uint8_t _status, const std::string& _body) {
        std::string out;
        put8(out, static_cast<uint8_t>(_service | 0x80));
        put8(out, 0);
        put8(out, _status);
        put8(out, 0);
        out.append(_body);
        return out;
    }

    // A tag against the table: the verbatim name first ("xxx[0]" is a legal point name), then the
    // parsed trailing indices. False = no such point; _syntax marks a tag that does not parse.
    bool lookup_point(cip_conn* _c, const std::string& _tag, tag_ref& _ref, point& _out,
                      bool& _syntax, std::string& _why) {
        _syntax = false;
        auto it = _c->points.find(_tag);
        if (it != _c->points.end()) { // the whole point, verbatim
            _ref = tag_ref();
            _ref.name = _tag;
            _out = it->second;
            return true;
        }
        if (!parse_tag_path(_tag, _ref, _why)) {
            _syntax = true;
            return false;
        }
        it = _c->points.find(_ref.name);
        if (it == _c->points.end()) {
            _why = "not declared";
            return false;
        }
        _out = it->second;
        return true;
    }

    // What a tag covers: the whole shape, a prefix (row-major makes a row contiguous), or a range.
    bool fit_count(const point& _p, const tag_ref& _r, uint32_t& _count, std::string& _why) {
        const int ndims = _p.dims[1] != 0 ? 2 : 1;
        if (_r.dims > ndims) {
            _why = describe(_p) + " takes " + (ndims == 1 ? "one index" : "two indices") +
                   ", the tag gives " + std::to_string(_r.dims);
            return false;
        }
        if (_p.type && _p.type->k != cip_type::kind::atomic) {
            if (_r.dims > 0) {
                _why = "a " + std::string(_p.type->name) + " is one value -- no index";
                return false;
            }
            _count = 1;
            return true;
        }
        if (_r.dims == ndims) {
            _count = _r.count; // one element, or the range the last index spelled
            return true;
        }
        _count = _r.dims == 0 ? _p.count : _p.dims[1]; // the whole shape, or a row of it
        return true;
    }

    // The flat offset a tag's indices select: row-major, so a prefix (a row) starts at a
    // stride. False = a shape that does not fit the point, or an index past its end.
    bool linear_start(const point& _p, const tag_ref& _r, uint32_t& _start, std::string& _why) {
        const bool two = _p.dims[1] != 0;
        if (_r.dims > (two ? 2 : 1)) {
            _why = describe(_p) + " takes " + (two ? "two indices" : "one index") +
                   ", the tag gives " + std::to_string(_r.dims);
            return false;
        }
        if (_r.dims == 0)
            _start = 0;
        else if (_r.dims == 1)
            _start = _r.index[0] * (two ? _p.dims[1] : 1);
        else
            _start = _r.index[0] * _p.dims[1] + _r.index[1];
        if (_start >= _p.count) {
            _why = describe(_p) + " has no element " + std::to_string(_start);
            return false;
        }
        return true;
    }

    // The offset and the run a whole tag covers.
    bool linear_from(const point& _p, const tag_ref& _r, uint32_t& _start, uint32_t& _count,
                     std::string& _why) {
        if (!fit_count(_p, _r, _count, _why)) return false;
        if (!linear_start(_p, _r, _start, _why)) return false;
        if (_count > _p.count - _start) {
            _why = describe(_p) + " has no element " + std::to_string(_start);
            return false;
        }
        return true;
    }

    // A flat run in its declared 2-D shape (row-major).
    alx::variant nest_2d(const point& _p, alx::varvec&& _flat) {
        alx::varvec rows;
        for (uint32_t i = 0; i < _p.dims[0]; ++i) {
            alx::varvec row;
            for (uint32_t j = 0; j < _p.dims[1]; ++j)
                row.push_back(std::move(_flat[i * _p.dims[1] + j]));
            rows.push_back(alx::variant(std::move(row)));
        }
        return alx::variant(std::move(rows));
    }

    // A whole-point value in either spelling: the nested shape is flattened row-major, a flat
    // run is taken as it is.
    bool flatten_2d(const point& _p, const alx::varvec& _v, alx::varvec& _out, std::string& _why) {
        if (_v.empty() || !_v[0].is<alx::varvec>()) {
            _out = _v;
            return true;
        }
        if (_v.size() != _p.dims[0]) {
            _why = "declared " + describe(_p) + ", the value has " + std::to_string(_v.size()) +
                   " rows";
            return false;
        }
        for (const alx::variant& row : _v) {
            const alx::varvec& r = row.to<alx::varvec>();
            if (r.size() != _p.dims[1]) {
                _why = "declared " + describe(_p) + ", a row has " + std::to_string(r.size()) +
                       " elements";
                return false;
            }
            for (const alx::variant& e : r) _out.push_back(e);
        }
        return true;
    }

    alx::variant decode_value(const cip_type& _t, const uint8_t* _data) {
        if (_t.k == cip_type::kind::str16 || _t.k == cip_type::kind::str8) {
            const size_t at = _t.k == cip_type::kind::str16 ? 2 : 1;
            const size_t len = _t.k == cip_type::kind::str16 ? get16(_data) : _data[0];
            return alx::variant(std::string(reinterpret_cast<const char*>(_data + at), len));
        }
        switch (_t.code) {
        case 0xC1: return alx::variant(_data[0] != 0);
        case 0xC2: return alx::variant(static_cast<alx::int_64>(static_cast<int8_t>(_data[0])));
        case 0xC3:
            return alx::variant(static_cast<alx::int_64>(static_cast<int16_t>(get16(_data))));
        case 0xC4:
            return alx::variant(static_cast<alx::int_64>(static_cast<int32_t>(get32(_data))));
        case 0xC5: {
            // Unsigned throughout: shifting a signed 64-bit value into its sign bit is UB
            const uint64_t bits = static_cast<uint64_t>(get32(_data)) |
                                  (static_cast<uint64_t>(get32(_data + 4)) << 32);
            return alx::variant(static_cast<alx::int_64>(bits));
        }
        case 0xCA: return alx::variant(static_cast<double>(get_float(_data)));
        case 0xCB: {
            const uint64_t bits = static_cast<uint64_t>(get32(_data)) |
                                  (static_cast<uint64_t>(get32(_data + 4)) << 32);
            double v = 0;
            std::memcpy(&v, &bits, sizeof(v));
            return alx::variant(v);
        }
        case 0xD1: return alx::variant(static_cast<alx::int_64>(_data[0]));
        default: return alx::variant();
        }
    }

    bool pack_value(const cip_type& _type, const alx::variant& _v, std::string& _data,
                    std::string& _why) {
        if (_type.k != cip_type::kind::atomic) {
            if (!_v.is<std::string>()) {
                _why = "a " + std::string(_type.name) + " point takes a string";
                return false;
            }
            const std::string& text = _v.to<std::string>();
            if (_type.k == cip_type::kind::str16) {
                if (text.size() > 0xFFFF) { // the wire's own ceiling: the length field is 2 bytes
                    _why = "a STRING carries at most 65535 characters";
                    return false;
                }
                put16(_data, static_cast<uint16_t>(text.size()));
                _data.append(text);
                if (text.size() % 2) put8(_data, 0); // odd length: one pad, as the sim wants
            } else {
                if (text.size() > 0xFF) { // one byte of length
                    _why = "an SSTRING carries at most 255 characters";
                    return false;
                }
                put8(_data, static_cast<uint8_t>(text.size()));
                _data.append(text);
            }
            return true;
        }
        switch (_type.code) {
        case 0xC1:
            if (!_v.is<bool>()) {
                _why = "a BOOL point takes a bool";
                return false;
            }
            put8(_data, _v.to<bool>() ? 1 : 0);
            return true;
        case 0xD1:
        case 0xC2:
        case 0xC3:
        case 0xC4:
        case 0xC5: {
            if (!_v.is<alx::int_64>()) {
                _why = "an integer point takes an int";
                return false;
            }
            const int64_t n = _v.to<alx::int_64>();
            if (_type.code == 0xD1 && (n < 0 || n > 255)) {
                _why = "a BYTE point takes an int 0 to 255";
                return false;
            }
            switch (_type.width) {
            case 1: put8(_data, static_cast<uint8_t>(n)); break;
            case 2: put16(_data, static_cast<uint16_t>(n)); break;
            case 4: put32(_data, static_cast<uint32_t>(n)); break;
            default:
                put32(_data, static_cast<uint32_t>(n));
                put32(_data, static_cast<uint32_t>(static_cast<uint64_t>(n) >> 32));
                break;
            }
            return true;
        }
        case 0xCA:
        case 0xCB: {
            if (!_v.is<double>()) {
                _why = "a REAL point takes a float";
                return false;
            }
            if (_type.code == 0xCA) {
                put_float(_data, static_cast<float>(_v.to<double>()));
            } else {
                uint64_t bits = 0;
                const double d = _v.to<double>();
                std::memcpy(&bits, &d, sizeof(bits));
                put32(_data, static_cast<uint32_t>(bits));
                put32(_data, static_cast<uint32_t>(bits >> 32));
            }
            return true;
        }
        default:
            _why = "unsupported type";
            return false;
        }
    }

    // A byte run's value as its bytes: a bytes value (exactly the run), a string (a text field:
    // the rest of the run zeroes), or ints 0 to 255 (one per byte of the run).
    bool byte_run(const point& _p, uint32_t _count, const alx::variant& _v, std::string& _out,
                  std::string& _why) {
        if (_v.is<alx::bytes>()) {
            const alx::bytes& b = _v.to<alx::bytes>();
            if (b.size() != _count) {
                _why = "the run holds " + std::to_string(_count) + " bytes, the value has " +
                       std::to_string(b.size());
                return false;
            }
            _out.assign(reinterpret_cast<const char*>(b.cdata()), b.size());
            return true;
        }
        if (_v.is<std::string>()) {
            const std::string& s = _v.to<std::string>();
            if (s.size() > _count) {
                _why = "the run holds " + std::to_string(_count) + " bytes, the text has " +
                       std::to_string(s.size());
                return false;
            }
            _out = s;
            _out.append(_count - s.size(), '\0'); // a text field: the rest zeroes
            return true;
        }
        const alx::varvec* vs = _v.is<alx::varvec>() ? &_v.to<alx::varvec>() : nullptr;
        if (vs) {
            if (vs->size() != _count) {
                _why = "the run holds " + std::to_string(_count) + " bytes, the vec has " +
                       std::to_string(vs->size());
                return false;
            }
        } else if (_count != 1) {
            _why = "a " + describe(_p) + " run takes bytes, a string or ints 0 to 255";
            return false;
        }
        for (uint32_t i = 0; i < _count; ++i)
            if (!pack_value(*_p.type, vs ? (*vs)[i] : _v, _out, _why)) return false;
        return true;
    }
} // namespace cip_link
