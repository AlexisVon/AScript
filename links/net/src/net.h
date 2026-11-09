#ifndef ASCRIPT_NET_H
#define ASCRIPT_NET_H

#include "ascript.h"

#include "atransmit.h"

#include <atomic>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// net link — TCP/UDP handles carrying deliveries into the script's yield points
namespace net_link {

    // net_handle — one transport plus the delivery plumbing: handler callbacks, in-flight
    // accounting and stats. Receiver threads belong to the transport (one per TCP connection);
    // a delivery is a call queued through fwrap::currying and serviced at the script's yield().
    class net_handle : public alx::noncopyable {
    public:
        net_handle(std::string _type, std::string _addr, const alx::varmap& _opts);
        ~net_handle();

        void open();
        void close();

        bool send(const alx::bytes& _data, alx::uint_64 _loc);
        void disconnect(alx::uint_64 _loc);
        alx::varmap status();

        void set_recv_handler(alx::script::fwrap::callback* _cb);
        void set_event_handler(alx::script::fwrap::callback* _cb);

        bool is_server() const { return m_server; }
        bool closed() const { return m_closed.load(); }
        std::string label() const { return m_type + " " + m_addr; }

    private:
        struct conn_state {
            std::deque<std::future<alx::script::result>> inflight;
        };

        void on_bytes(const alx::bytes_view& _data, alx::uint_64 _loc);
        void on_trans(alx::uint_64 _loc, bool _connect, const char* _text);

        std::string addr_of(alx::uint_64 _loc) const;
        std::shared_ptr<conn_state> conn_of(alx::uint_64 _loc);
        void settle(std::future<alx::script::result>& _f);
        void note_result(const alx::script::result& _r);
        void note_error(const std::string& _msg);
        void submit_event(alx::varvec _args);
        void reap_events();

    private:
        alx::transmit* m_trans{nullptr};
        std::string m_type;
        std::string m_addr;
        bool m_udp{false};
        bool m_server{false};
        bool m_client{false};
        size_t m_window{1};
        alx::uint_32 m_expiry_ms{0};
        size_t m_max_inflight{64};
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_closed{false};
        alx::script::fwrap::callback* m_recv_cb{nullptr};
        alx::script::fwrap::callback* m_event_cb{nullptr};

        std::atomic<alx::uint_64> m_rx{0};
        std::atomic<alx::uint_64> m_delivered{0};
        std::atomic<alx::uint_64> m_expired{0};
        std::atomic<alx::uint_64> m_full{0};
        std::atomic<alx::uint_64> m_errors{0};
        mutable std::mutex m_err_mtx;
        std::string m_last_error;

        mutable std::mutex m_evt_mtx;
        std::deque<std::future<alx::script::result>> m_events;

        mutable std::mutex m_conn_mtx;
        std::unordered_map<alx::uint_64, std::shared_ptr<conn_state>> m_conns;
        std::shared_ptr<conn_state> m_udp_state;
    };

} // namespace net_link

#endif // !ASCRIPT_NET_H
