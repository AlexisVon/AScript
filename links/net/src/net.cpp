#include "net.h"

#include <chrono>
#include <thread>

// net link — TCP/UDP through alxcomm's transmit layer. Data and connection events reach the
// script as deliveries (fwrap::currying), serviced at the script's own yield() points; TCP
// blocks its receiver on the delivery futures (windowed backpressure), UDP drops on expiry.
namespace net_link {
    namespace {

        constexpr auto k_slice = std::chrono::milliseconds(50);

        bool is_text(const alx::variant& _v, const char* _s) {
            return _v.is<std::string>() && _v.to<std::string>() == _s;
        }

        alx::uint_64 u64_opt(const alx::varmap& _m, const char* _key, alx::uint_64 _def,
                             const char* _fn) {
            if (!_m.contain(_key)) return _def;
            const alx::variant& v = _m.value(_key);
            if (!v.is<alx::int_64>())
                throw alx::script::script_exception{
                    alx::script::error_type::ArgError,
                    std::string(_fn) + ": " + _key + " must be an int"};
            alx::int_64 n = v.to<alx::int_64>();
            return n < 0 ? 0 : static_cast<alx::uint_64>(n);
        }

        // A non-handle must raise here: the cast dereferences the register tag.
        template <typename T>
        T* as_handle(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::anyptr>())
                throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                    _desc + ": expected handle"};
            T* p = alx::anyptr_ex<T>::as(_v.as<alx::anyptr>());
            if (!p)
                throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                    _desc + ": handle type mismatch"};
            return p;
        }

        std::string text_of(const alx::variant& _v) {
            if (_v.null()) return "null";
            if (_v.is<bool>()) return _v.to<bool>() ? "true" : "false";
            if (_v.is<alx::int_64>()) return std::to_string(_v.to<alx::int_64>());
            if (_v.is<double>()) return std::to_string(_v.to<double>());
            if (_v.is<std::string>()) return _v.to<std::string>();
            if (_v.is<alx::bytes>()) return "bytes";
            if (_v.is<alx::varvec>()) return "vec";
            return "?";
        }

    } // namespace

    net_handle::net_handle(std::string _type, std::string _addr, const alx::varmap& _opts)
        : m_type(std::move(_type)), m_addr(std::move(_addr)) {
        if (m_type != "tcp_server" && m_type != "tcp_client" && m_type != "udp_trans")
            throw alx::script::script_exception{
                alx::script::error_type::ArgError,
                "net.open: expected tcp_server, tcp_client or udp_trans, got " + m_type};
        if (alx::transmit::to_strip(alx::transmit::from_strip64(m_addr)) != m_addr)
            throw alx::script::script_exception{
                alx::script::error_type::ArgError,
                "net.open: address must be a.b.c.d:port, got " + m_addr};
        m_udp = m_type == "udp_trans";
        m_server = m_type == "tcp_server";
        m_client = m_type == "tcp_client";

        m_window = static_cast<size_t>(u64_opt(_opts, "window", 1, "net.open"));
        if (m_window == 0) m_window = 1;
        m_max_inflight = static_cast<size_t>(u64_opt(_opts, "max_inflight", 64, "net.open"));
        if (m_max_inflight == 0) m_max_inflight = 1;
        m_expiry_ms = m_udp ? static_cast<alx::uint_32>(u64_opt(_opts, "expiry", 100, "net.open")) : 0;

        m_trans = alx::transmit::create(m_type, m_addr);
        if (!m_trans)
            throw alx::script::script_exception{
                alx::script::error_type::ArgError,
                "net.open: cannot create transport " + m_type};

        m_trans->bytes_recv.connect([this](const alx::bytes_view& _d, alx::uint_64 _loc) {
            on_bytes(_d, _loc);
        });
        m_trans->trans_mesg.connect([this](alx::uint_64 _loc, bool _connect, const char* _text) {
            on_trans(_loc, _connect, _text);
        });
        if (m_udp) m_udp_state = std::make_shared<conn_state>();
    }

    net_handle::~net_handle() {
        close();
    }

    void net_handle::open() {
        if (!m_trans->open()) {
            std::string err;
            {
                std::lock_guard<std::mutex> g(m_err_mtx);
                err = m_last_error;
            }
            throw alx::script::script_exception{
                alx::script::error_type::RuntimeError,
                "net.open: " + (err.empty() ? m_addr : err)};
        }
    }

    void net_handle::close() {
        if (m_closed.exchange(true)) return;
        m_stop.store(true);
        if (m_trans) m_trans->close(0);
        delete m_recv_cb;
        m_recv_cb = nullptr;
        delete m_event_cb;
        m_event_cb = nullptr;
        delete m_trans;
        m_trans = nullptr;
    }

    bool net_handle::send(const alx::bytes& _data, alx::uint_64 _loc) {
        if (m_closed.load() || m_stop.load() || !m_trans) return false;
        return m_trans->bytes_send(_data.data(), _data.size(), _loc);
    }

    void net_handle::disconnect(alx::uint_64 _loc) {
        if (m_closed.load() || !m_trans)
            throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                "net.disconnect: handle closed"};
        if (!m_server)
            throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                "net.disconnect: only a server handle has peers"};
        m_trans->close(_loc);
    }

    alx::varmap net_handle::status() {
        reap_events();
        alx::varmap m;
        m["type"] = alx::variant(m_type);
        m["addr"] = alx::variant(m_addr);
        m["closed"] = alx::variant(m_closed.load());
        m["peers"] = alx::variant(static_cast<alx::int_64>(m_trans ? m_trans->connect_num() : 0));
        m["rx"] = alx::variant(static_cast<alx::int_64>(m_rx.load()));
        m["delivered"] = alx::variant(static_cast<alx::int_64>(m_delivered.load()));
        m["expired"] = alx::variant(static_cast<alx::int_64>(m_expired.load()));
        m["full"] = alx::variant(static_cast<alx::int_64>(m_full.load()));
        m["errors"] = alx::variant(static_cast<alx::int_64>(m_errors.load()));
        {
            std::lock_guard<std::mutex> g(m_err_mtx);
            m["last_error"] = alx::variant(m_last_error);
        }
        return m;
    }

    void net_handle::set_recv_handler(alx::script::fwrap::callback* _cb) {
        delete m_recv_cb;
        m_recv_cb = _cb;
    }

    void net_handle::set_event_handler(alx::script::fwrap::callback* _cb) {
        delete m_event_cb;
        m_event_cb = _cb;
    }

    void net_handle::on_bytes(const alx::bytes_view& _data, alx::uint_64 _loc) {
        if (m_stop.load()) return;
        ++m_rx;
        if (!m_recv_cb) return;
        try {
            auto st = conn_of(_loc);
            alx::varvec args;
            args.push_back(alx::variant(addr_of(_loc)));
            args.push_back(alx::variant(alx::bytes(_data.data(), _data.size())));

            if (m_udp) {
                while (!st->inflight.empty() && st->inflight.front().wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    settle(st->inflight.front());
                    st->inflight.pop_front();
                }
                if (st->inflight.size() >= m_max_inflight) {
                    ++m_full;
                    return;
                }
                st->inflight.push_back(m_recv_cb->call(args, 0, m_expiry_ms));
                return;
            }

            // TCP: submit least-recently, then wait on the oldest future in interruptible slices
            std::future<alx::script::result> f;
            for (;;) {
                f = m_recv_cb->call(args, 0, 0);
                if (f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) break;
                auto r = f.get();
                if (r.error == alx::script::error_type::YieldError && is_text(r.value, "yield full")) {
                    for (int i = 0; i < 20 && !m_stop.load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    if (m_stop.load()) return;
                    continue;
                }
                note_result(r);
                return;
            }
            st->inflight.push_back(std::move(f));
            while (st->inflight.size() >= m_window) {
                auto& front = st->inflight.front();
                while (!m_stop.load() && front.wait_for(k_slice) == std::future_status::timeout) {
                }
                if (m_stop.load()) return;
                settle(front);
                st->inflight.pop_front();
            }
        } catch (...) {
            ++m_errors;
        }
    }

    void net_handle::on_trans(alx::uint_64 _loc, bool _connect, const char* _text) {
        try {
            const std::string text = _text ? _text : "";
            if (!_connect && text != "disconnect" && text != "close") note_error(text);
            if (!m_udp && !_connect && (_loc != 0 || m_client)) {
                std::lock_guard<std::mutex> g(m_conn_mtx);
                m_conns.erase(_loc);
            }
            if (m_stop.load() || !m_event_cb) return;
            if (_loc == 0) {
                if (_connect) return; // transport opened: the script learns this from $net.open
                if (!m_client) return; // server/udp transport closed by the script itself
            }
            alx::varvec args;
            args.push_back(alx::variant(addr_of(_loc)));
            args.push_back(alx::variant(_connect));
            submit_event(std::move(args));
        } catch (...) {
            ++m_errors;
        }
    }

    std::string net_handle::addr_of(alx::uint_64 _loc) const {
        return _loc != 0 ? alx::transmit::to_strip(_loc) : m_addr;
    }

    std::shared_ptr<net_handle::conn_state> net_handle::conn_of(alx::uint_64 _loc) {
        if (m_udp) return m_udp_state;
        std::lock_guard<std::mutex> g(m_conn_mtx);
        auto& p = m_conns[_loc];
        if (!p) p = std::make_shared<conn_state>();
        return p;
    }

    void net_handle::settle(std::future<alx::script::result>& _f) {
        try {
            note_result(_f.get());
        } catch (...) {
            ++m_errors;
        }
    }

    void net_handle::note_result(const alx::script::result& _r) {
        if (_r.error == alx::script::error_type::NoError) {
            ++m_delivered;
            return;
        }
        const std::string msg = _r.value.is<std::string>() ? _r.value.to<std::string>() : "";
        if (_r.error == alx::script::error_type::YieldError) {
            if (msg == "expired") {
                ++m_expired;
                return;
            }
            if (msg == "yield full") {
                ++m_full;
                return;
            }
        }
        ++m_errors;
        note_error(std::string(alx::script::error_type_name(_r.error)) + (msg.empty() ? "" : ": " + msg));
    }

    void net_handle::note_error(const std::string& _msg) {
        std::lock_guard<std::mutex> g(m_err_mtx);
        m_last_error = _msg;
    }

    void net_handle::submit_event(alx::varvec _args) {
        for (int i = 0; i < 20 && !m_stop.load(); ++i) {
            auto f = m_event_cb->call(_args, 1, 0);
            if (f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                std::lock_guard<std::mutex> g(m_evt_mtx);
                m_events.push_back(std::move(f));
                return;
            }
            auto r = f.get();
            if (r.error == alx::script::error_type::YieldError && is_text(r.value, "yield full")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            note_result(r);
            return;
        }
        ++m_full;
    }

    void net_handle::reap_events() {
        std::lock_guard<std::mutex> g(m_evt_mtx);
        while (!m_events.empty() && m_events.front().wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            try {
                note_result(m_events.front().get());
            } catch (...) {
                ++m_errors;
            }
            m_events.pop_front();
        }
    }

    namespace net_impl {

        static std::string str_arg(alx::script::fwrap& fw, size_t _idx, const char* _fn) {
            if (fw.size() <= _idx || !fw[_idx].is<std::string>())
                throw alx::script::script_exception{
                    alx::script::error_type::ArgError,
                    std::string(_fn) + ": expected a string at argument " + std::to_string(_idx + 1)};
            return fw[_idx].to<std::string>();
        }

        static alx::uint_64 addr_arg(alx::script::fwrap& fw, size_t _idx, const char* _fn) {
            const std::string s = str_arg(fw, _idx, _fn);
            alx::uint_64 loc = alx::transmit::from_strip64(s);
            if (loc == 0 && s != "0.0.0.0:0")
                throw alx::script::script_exception{
                    alx::script::error_type::ConvError,
                    std::string(_fn) + ": invalid address '" + s + "'"};
            return loc;
        }

        static void fn_open(alx::script::fwrap& fw) {
            std::string type = str_arg(fw, 0, "net.open");
            std::string addr = str_arg(fw, 1, "net.open");
            alx::varmap opts;
            if (fw.size() > 2 && fw[2].is<alx::varmap>()) opts = fw[2].to<alx::varmap>();

            auto h = std::make_unique<net_handle>(type, addr, opts);
            h->open();
            fw.freturn(alx::variant(alx::anyptr_ex<net_handle>::make_ref(h.release())));
        }

        static void fn_on_recv(alx::script::fwrap& fw) {
            auto* h = as_handle<net_handle>(fw[0], "net.on_recv: handle from net.open");
            if (h->closed())
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "net.on_recv: handle closed"};
            h->set_recv_handler(fw.currying(str_arg(fw, 1, "net.on_recv")));
        }

        static void fn_on_event(alx::script::fwrap& fw) {
            auto* h = as_handle<net_handle>(fw[0], "net.on_event: handle from net.open");
            if (h->closed())
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "net.on_event: handle closed"};
            h->set_event_handler(fw.currying(str_arg(fw, 1, "net.on_event")));
        }

        static void fn_send(alx::script::fwrap& fw) {
            auto* h = as_handle<net_handle>(fw[0], "net.send: handle from net.open");
            alx::uint_64 loc = 0;
            if (fw.size() > 2) loc = addr_arg(fw, 2, "net.send");

            if (fw[1].is<alx::bytes>()) {
                fw.freturn(alx::variant(h->send(fw[1].to<alx::bytes>(), loc)));
            } else if (fw[1].is<std::string>()) {
                alx::bytes data(fw[1].to<std::string>());
                fw.freturn(alx::variant(h->send(data, loc)));
            } else {
                throw alx::script::script_exception{
                    alx::script::error_type::ArgError,
                    "net.send: expected bytes or string at argument 2"};
            }
        }

        static void fn_disconnect(alx::script::fwrap& fw) {
            auto* h = as_handle<net_handle>(fw[0], "net.disconnect: handle from net.open");
            alx::uint_64 loc = addr_arg(fw, 1, "net.disconnect");
            if (loc == 0)
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "net.disconnect: an address is required"};
            h->disconnect(loc);
            fw.freturn(alx::variant(true));
        }

        static void fn_status(alx::script::fwrap& fw) {
            auto* h = as_handle<net_handle>(fw[0], "net.status: handle from net.open");
            fw.freturn(alx::variant(h->status()));
        }

        static void fn_close(alx::script::fwrap& fw) {
            auto* h = as_handle<net_handle>(fw[0], "net.close: handle from net.open");
            h->close();
        }

        static std::string net_desc(const alx::anyptr& _p) {
            net_handle* h = alx::anyptr_ex<net_handle>::as(_p);
            return h ? h->label() : std::string();
        }

        static alx::variant net_view(const alx::anyptr& _p) {
            net_handle* h = alx::anyptr_ex<net_handle>::as(_p);
            if (!h) return alx::variant();
            const alx::varmap st = h->status();
            std::string text;
            for (auto it = st.begin(); it != st.end(); ++it) {
                text += it.key();
                text += ": ";
                text += text_of(it.value());
                text += "\n";
            }
            alx::varmap m;
            m["kind"] = alx::variant(std::string("text"));
            m["format"] = alx::variant(std::string("plain"));
            m["data"] = alx::variant(alx::bytes(text.data(), text.size()));
            return alx::variant(std::move(m));
        }

    } // namespace net_impl

    extern "C"
#ifdef _WIN32
        __declspec(dllexport)
#endif
        void alexis_script_load(alx::script::fwrap& _fw) {
        _fw.bind(alx::anyptr_ex<net_handle>::rid, "net", net_impl::net_desc, net_impl::net_view);
        _fw.bind("open", net_impl::fn_open);
        _fw.bind("on_recv", net_impl::fn_on_recv);
        _fw.bind("on_event", net_impl::fn_on_event);
        _fw.bind("send", net_impl::fn_send);
        _fw.bind("disconnect", net_impl::fn_disconnect);
        _fw.bind("status", net_impl::fn_status);
        _fw.bind("close", net_impl::fn_close);
    }

} // namespace net_link
