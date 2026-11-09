// Copyright (c) 2026 AlexisVon
// SPDX-License-Identifier: Apache-2.0

#ifndef ASCRIPT_CIP_INTERNAL_H
#define ASCRIPT_CIP_INTERNAL_H

#include "acomm.h" // alx::comm: the server's transport base
#include "atransmit.h" // alx::transmit: the handle's transport
#include "avariant.h" // alx::variant / bytes / varvec

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

/// The cip link's private API: what more than one of its translation units needs. One section per
/// defining file, declarations in the order that file defines them.
namespace cip_link {

    struct cip_conn; // the handle, defined with the connection below

    // ---------------------------------------------------------------------------
    // wire.cpp: the wire's vocabulary.
    // ---------------------------------------------------------------------------

    void dbg(const std::string& _what);

    void put8(std::string& _b, uint8_t _v);
    void put16(std::string& _b, uint16_t _v);
    void put32(std::string& _b, uint32_t _v);
    uint16_t get16(const uint8_t* _p);
    uint32_t get32(const uint8_t* _p);

    struct cip_type {
        /// atomic = a fixed-width value; str16/str8 = the simulator's string flavors: a length
        /// field (2 or 1 bytes) then the characters, no fixed capacity on the wire.
        enum class kind : uint8_t { atomic,
                                    str16,
                                    str8 };
        const char* name;
        uint16_t code;
        size_t width; // atomics only
        kind k = kind::atomic;
    };
    extern const cip_type kTypes[]; // the table itself is wire.cpp's

    const cip_type* named(const char* _name);
    const cip_type* type_by_code(uint16_t _code);

    void seg_class(std::string& _p, uint32_t _id);
    void seg_instance(std::string& _p, uint32_t _id);
    void seg_port_link(std::string& _p, uint8_t _port, uint8_t _link);
    void put_path(std::string& _b, const std::string& _segments);
    void put_route_path(std::string& _b, const std::string& _segments);

    // A tag: the point's verbatim name plus a trailing index run -- `Tag[3]`, `Tag[r][c]`,
    // `Tag[a,b)`, range only on the last index.
    struct tag_ref {
        std::string name; // the point, verbatim
        uint32_t index[2] = {0, 0}; // the trailing indices, outermost first
        int dims = 0; // how many indices the tag spelled; 0 = the whole point
        uint32_t count = 1; // the range's length where the last index carried one
    };

    bool parse_tag_path(const std::string& _tag, tag_ref& _out, std::string& _why);
    bool tag_path_segments(const tag_ref& _ref, std::string& _segments, std::string& _why);

    struct point {
        const cip_type* type = nullptr;
        uint32_t count = 1; // atomics: the element count; a string is one value
        uint32_t dims[2] = {0, 0}; // a 2-D shape: dims[1] != 0 means dims[0] x dims[1]
    };

    bool by_byte(const point& _p);
    std::string describe(const point& _p);

    // A CIP reply header, 4 bytes as the simulator emits it: service, reserved, status,
    // extended-status size in words (the spec's own header is 3 bytes -- measured, not guessed).
    struct cip_status {
        uint8_t service = 0;
        size_t data_at = 0; // where the service's own data starts
    };

    bool read_status(const std::string& _cip, cip_status& _st, std::string& _why);
    std::string cip_reply(uint8_t _service, uint8_t _status, const std::string& _body);

    bool lookup_point(cip_conn* _c, const std::string& _tag, tag_ref& _ref, point& _out,
                      bool& _syntax, std::string& _why);

    bool fit_count(const point& _p, const tag_ref& _r, uint32_t& _count, std::string& _why);
    bool linear_start(const point& _p, const tag_ref& _r, uint32_t& _start, std::string& _why);
    bool linear_from(const point& _p, const tag_ref& _r, uint32_t& _start, uint32_t& _count,
                     std::string& _why);
    alx::variant nest_2d(const point& _p, alx::varvec&& _flat);
    bool flatten_2d(const point& _p, const alx::varvec& _v, alx::varvec& _out, std::string& _why);

    // A request budget: the reply rides in one UCMM message and a device caps the response near
    // 500 bytes -- a request that cannot fit is refused here, by name.
    constexpr size_t kMaxPayloadBytes = 480;

    alx::variant decode_value(const cip_type& _t, const uint8_t* _data);
    bool pack_value(const cip_type& _type, const alx::variant& _v, std::string& _data,
                    std::string& _why);
    bool byte_run(const point& _p, uint32_t _count, const alx::variant& _v, std::string& _out,
                  std::string& _why);

    // ---------------------------------------------------------------------------
    // The connection: one TCP transport, one outstanding request at a time.
    // ---------------------------------------------------------------------------

    struct reply_t {
        uint16_t cmd = 0;
        uint32_t session = 0;
        uint32_t status = 0; // the encapsulation status word
        uint64_t ctx = 0; // the sender context, echoed: the exchange that owns this reply
        std::string payload;
    };

    // The server's transport: comm owns the worker thread and the send queue, so a reply can
    // be queued from inside the receive handler (alxcomm forbids sending from a transmit handler).
    struct server_trans : public alx::comm {
        cip_conn* c = nullptr;
        server_trans(alx::transmit* _t, cip_conn* _c) : alx::comm(_t), c(_c) {}
        void on_bytes_recv(const alx::bytes_view& _data, alx::uint_64 _loc) override;
    };

    struct cip_conn {
        alx::transmit* tr = nullptr;
        server_trans* srv = nullptr; // server handles own their transport through comm
        std::string addr; // "ip:port", for messages
        std::string route; // route-path segments (default: port 1, link 0)
        std::string send; // send-path segments (default: class 6 instance 1)

        uint32_t session = 0;
        uint32_t seq = 1;
        int timeout_ms = 3000;

        /// Serializes a whole exchange (seq, send, reply wait) across threads.
        std::mutex io_mtx;

        /// The declared points: name -> type + shape. A write needs one; a read only checks
        /// against it when the name is declared.
        std::map<std::string, point> points;

        /// Server side: the same declaration table doubles as the local point table, and
        /// `locals` holds its values.
        bool server = false;
        std::map<std::string, alx::variant> locals;
        std::map<alx::uint_64, std::string> peers; // peer loc -> its accumulated request bytes
        std::map<alx::uint_64, uint32_t> peer_session; // peer loc -> the session it registered
        uint32_t next_session = 1;
        std::mutex peer_mtx; // guards peers / peer_session; a different lock from mtx

        std::mutex mtx; // guards rx/reply and the reply wait; io_mtx above orders whole exchanges
        std::condition_variable cv;
        std::string rx; // accumulated response bytes
        reply_t reply; // the last complete reply
        bool reply_ok = false;
        std::string last_err; // the transport's own words (open failure, send failure)
        std::string reason; // why the last get/set failed (empty after one that succeeded)
        bool broken = false; // a frame that cannot be one of ours: the stream lost its framing

        ~cip_conn() {
            if (srv) {
                srv->close();
                delete srv;
            }
            if (tr) {
                tr->close();
                delete tr;
            }
        }
    };

    // ---------------------------------------------------------------------------
    // client.cpp: one side of the wire.
    // ---------------------------------------------------------------------------

    cip_conn* open_conn(const std::string& _addr, int _timeout_ms, std::string& _why);
    bool read_tag(cip_conn* _c, const std::string& _tag, alx::variant& _out, bool& _script_err,
                  std::string& _why);
    bool write_tag(cip_conn* _c, const std::string& _tag, const alx::variant& _v, const point& _p,
                   bool& _script_err, std::string& _why);

    // ---------------------------------------------------------------------------
    // server.cpp: the other side -- the local point table the wire reads and writes.
    // ---------------------------------------------------------------------------

    alx::variant zero_of(const point& _p);
    bool local_read_locked(cip_conn* _c, const std::string& _tag, alx::variant& _out,
                           bool& _script_err, std::string& _why);
    bool local_read(cip_conn* _c, const std::string& _tag, alx::variant& _out, bool& _script_err,
                    std::string& _why);
    bool local_write_locked(cip_conn* _c, const std::string& _tag, const alx::variant& _v,
                            bool& _script_err, std::string& _why);
    bool local_write(cip_conn* _c, const std::string& _tag, const alx::variant& _v,
                     bool& _script_err, std::string& _why);

} // namespace cip_link

#endif // ASCRIPT_CIP_INTERNAL_H
