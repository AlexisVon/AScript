#include "hook.h"

#include "core/extend.h"
#include "core/inspect.h"
#include "snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iterator>
#include <string>
#include <utility>

namespace runner {
    namespace {
        // text — payload string field
        std::string text_field(const alx::varmap& _p, const char* _key) {
            if (!_p.contain(_key)) return std::string();
            return _p.value(_key).to<std::string>();
        }

        // int_field — payload numeric field
        alx::int_64 int_field(const alx::varmap& _p, const char* _key) {
            if (!_p.contain(_key)) return 0;
            return _p.value(_key).to<alx::int_64>(0);
        }

        // trim — strip surrounding whitespace
        std::string trim(const std::string& _s) {
            const size_t b = _s.find_first_not_of(" \t");
            if (b == std::string::npos) return std::string();
            const size_t e = _s.find_last_not_of(" \t");
            return _s.substr(b, e - b + 1);
        }

        // short_label — a trap label cut at a UTF-8 boundary
        std::string short_label(const std::string& _s) {
            constexpr size_t kMax = 48;
            if (_s.size() <= kMax) return _s;
            size_t cut = kMax;
            while (cut > 0 && (static_cast<unsigned char>(_s[cut]) & 0xC0) == 0x80) --cut;
            return _s.substr(0, cut) + "…";
        }

        // lookup — a local of the innermost frame, else the root scope
        const alx::variant* lookup(alx::script::hook_info& _info, const std::string& _name) {
            const alx::uint_64 depth = _info.wkfm_size();
            if (depth) {
                const alx::variant* v = _info.wkfm_data_cptr(depth - 1, _name.c_str());
                if (v) return v;
            }
            const void* root = _info.wken_root();
            return root ? _info.wken_data_cptr(root, _name.c_str()) : nullptr;
        }

        // truthy — a value's truth value for a bare condition
        bool truthy(const alx::variant& _v) {
            if (_v.null()) return false;
            if (_v.is<bool>()) return _v.to<bool>();
            if (_v.is_integer()) return _v.to_integer() != 0;
            if (_v.is_uinteger()) return _v.to_uinteger() != 0;
            if (_v.is_number()) return _v.to_number() != 0.0;
            if (_v.is<std::string>()) return !_v.to<std::string>().empty();
            if (_v.is<alx::bytes>()) return _v.to<alx::bytes>().size() != 0;
            if (_v.is<alx::varmap>()) return !_v.to<alx::varmap>().empty();
            if (_v.is<alx::varvec>()) return !_v.to<alx::varvec>().empty();
            if (_v.is<alx::varlst>()) return !_v.to<alx::varlst>().empty();
            return true;
        }

        // compare — one comparison; false when the types do not compare
        bool compare(const alx::variant& _v, const std::string& _op, const alx::variant& _lit) {
            if ((_v.is_number() || _v.is<bool>()) && (_lit.is_number() || _lit.is<bool>())) {
                const double a = _v.is<bool>() ? (_v.to<bool>() ? 1.0 : 0.0) : _v.to_number();
                const double b = _lit.is<bool>() ? (_lit.to<bool>() ? 1.0 : 0.0) : _lit.to_number();
                if (_op == "==") return a == b;
                if (_op == "!=") return a != b;
                if (_op == ">") return a > b;
                if (_op == "<") return a < b;
                if (_op == ">=") return a >= b;
                if (_op == "<=") return a <= b;
                return false;
            }
            if (_v.is<std::string>() && _lit.is<std::string>()) {
                const std::string& a = _v.to<std::string>();
                const std::string& b = _lit.to<std::string>();
                if (_op == "==") return a == b;
                if (_op == "!=") return a != b;
                if (_op == ">") return a > b;
                if (_op == "<") return a < b;
                if (_op == ">=") return a >= b;
                if (_op == "<=") return a <= b;
            }
            return false;
        }

        // parse_literal — "text" / true / false / integer / float
        alx::variant parse_literal(const std::string& _s) {
            if (_s.size() >= 2 && _s.front() == '"' && _s.back() == '"')
                return alx::variant(_s.substr(1, _s.size() - 2));
            if (_s == "true") return alx::variant(true);
            if (_s == "false") return alx::variant(false);
            char* end = nullptr;
            const double d = std::strtod(_s.c_str(), &end);
            if (end && *end == '\0' && end != _s.c_str()) return alx::variant(d);
            return alx::variant(_s);
        }

        // condition_holds — "name" or "name op literal"; missing name means false
        bool condition_holds(alx::script::hook_info& _info, const std::string& _expr) {
            const std::string expr = trim(_expr);
            if (expr.empty()) return true;
            static const char* kOps[] = {"==", "!=", ">=", "<=", ">", "<"};
            std::string op;
            size_t pos = std::string::npos;
            for (const char* opText : kOps) {
                const size_t at = expr.find(opText);
                if (at == std::string::npos) continue;
                if (at < pos || (at == pos && std::strlen(opText) > op.size())) {
                    pos = at;
                    op = opText;
                }
            }
            const std::string name = trim(pos == std::string::npos ? expr : expr.substr(0, pos));
            const alx::variant* v = lookup(_info, name);
            if (!v) return false;
            if (op.empty()) return truthy(*v);
            const alx::variant lit = parse_literal(trim(expr.substr(pos + op.size())));
            return compare(*v, op, lit);
        }

        // parse_step — "in" | "over" | "out"; "over" for anything else
        std::string parse_step(const alx::varmap& _p) {
            const std::string kind = text_field(_p, "kind");
            return kind == "in" ? kind : kind == "out" ? kind
                                                       : std::string("over");
        }
    } // namespace

    hook::hook(control& _ctl, writer& _out, source_registry& _src)
        : m_ctl(_ctl), m_out(_out), m_src(_src) {
        m_main.owner = this;
        m_main.id = 0;
        m_main.parent = -1;
    }

    void hook::configure(const proto::breakpoint_list& _bps, bool _stop_on_entry, bool _skip_traps,
                         bool _multi_thread) {
        m_bps = _bps;
        m_bp_views.clear();
        m_step = step_mode::none;
        m_stop_on_entry = _stop_on_entry;
        m_skip_traps.store(_skip_traps);
        m_multi_thread.store(_multi_thread);
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_main.tid = std::this_thread::get_id();
            m_tids[m_main.tid] = 0;
            m_run_active.store(true);
            m_exit.store(false);
            m_stopped.store(false);
            m_trigger = 0;
        }
        m_cv.notify_all();
    }

    void hook::finish(bool _console) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_run_active.store(false);
            m_stopped.store(false);
            m_trigger = 0;
            if (!_console) m_exit.store(true);
        }
        m_ctl.clear_debug();
        m_ctl.set_debug_active(false);
        m_cv.notify_all();
    }

    void hook::shutdown() {
        finish(false);
    }

    void hook::child_observer(void* _ud, bool _attach, alx::script::engine* _child,
                              std::thread::id _parent, std::thread::id _child_tid) {
        if (_ud) static_cast<hook*>(_ud)->observe(_attach, _child, _parent, _child_tid);
    }

    bool hook::entry(alx::script::hook_info& _info) {
        auto* ctx = static_cast<thread_ctx*>(_info.hkdt);
        if (!ctx || !ctx->owner) return true;
        return ctx->owner->dispatch(_info);
    }

    bool hook::dispatch(alx::script::hook_info& _info) {
        auto* ctx = static_cast<thread_ctx*>(_info.hkdt);
        if (!ctx) return true;
        return ctx->id == 0 ? dispatch_main(_info) : dispatch_child(_info, *ctx);
    }

    bool hook::dispatch_main(alx::script::hook_info& _info) {
        apply_pending_breakpoints();
        if (m_ctl.stop_requested()) {
            if (_info.desc) *_info.desc = "stopped by host";
            return false;
        }
        if (m_stopped.load(std::memory_order_relaxed)) return stop_wave(_info, m_main, "sync");
        switch (_info.type) {
        case alx::script::hook_event::exec:
            return on_exec(_info);
        case alx::script::hook_event::debug:
            return on_debug(_info);
        case alx::script::hook_event::trap:
            return on_trap(_info);
        default:
            return true;
        }
    }

    bool hook::dispatch_child(alx::script::hook_info& _info, thread_ctx& _ctx) {
        if (!m_multi_thread.load(std::memory_order_relaxed)) return true;
        if (m_ctl.stop_requested()) {
            if (_info.desc) *_info.desc = "stopped by host";
            return false;
        }
        if (m_exit.load(std::memory_order_relaxed)) return false;
        if (!m_run_active.load(std::memory_order_relaxed)) return true;
        if (_info.type == alx::script::hook_event::debug ||
            _info.type == alx::script::hook_event::trap) {
            if (m_ctl.pause_requested()) {
                m_ctl.clear_pause();
                return stop_wave(_info, _ctx, "pause");
            }
        }
        if (_info.type == alx::script::hook_event::trap) {
            if (m_skip_traps.load(std::memory_order_relaxed)) return true;
            std::string args;
            std::string label;
            if (_info.info.is<alx::varmap>()) {
                const alx::varmap& m = _info.info.to<alx::varmap>();
                if (m.contain("args")) {
                    const alx::variant& a = m.value("args");
                    args = core::utils::repr(a);
                    label = a.is<std::string>() ? a.to<std::string>() : short_label(args);
                }
            }
            return stop_wave(_info, _ctx, "trap", std::move(label), std::move(args));
        }
        if (_info.type == alx::script::hook_event::debug &&
            m_stopped.load(std::memory_order_relaxed))
            return stop_wave(_info, _ctx, "sync");
        return true;
    }

    bool hook::on_exec(alx::script::hook_info& _info) {
        if (m_ctl.pause_requested()) {
            m_ctl.clear_pause();
            return stop_wave(_info, m_main, "pause");
        }
        return true;
    }

    bool hook::on_debug(alx::script::hook_info& _info) {
        if (m_step != step_mode::none) {
            if (m_epoch.load(std::memory_order_relaxed) != m_step_epoch) {
                m_step = step_mode::none; // another resume cancelled the pending step
            } else {
                const alx::uint_64 depth = func_depth(_info);
                const bool hit = m_step == step_mode::in ||
                                 (m_step == step_mode::over && depth <= m_step_depth) ||
                                 (m_step == step_mode::out && depth < m_step_depth);
                if (!hit) return true;
                m_step = step_mode::none;
                return stop_wave(_info, m_main, "step");
            }
        }
        if (m_stop_on_entry) {
            m_stop_on_entry = false;
            return stop_wave(_info, m_main, "entry");
        }
        if (m_ctl.pause_requested()) {
            m_ctl.clear_pause();
            return stop_wave(_info, m_main, "pause");
        }
        if (!m_bps.empty()) {
            const alx::uint_64 row = _info.wkdt_row();
            const void* entity = current_entity(_info);
            if (row) {
                const proto::breakpoint* bp = breakpoint_at(_info, entity, row);
                if (bp) {
                    if (!bp->condition.empty() && !condition_holds(_info, bp->condition)) return true;
                    if (!bp->log.empty()) {
                        m_out.output(bp->log + "\n");
                        return true; // logpoint: no pause
                    }
                    return stop_wave(_info, m_main, "breakpoint");
                }
            }
        }
        return true;
    }

    bool hook::on_trap(alx::script::hook_info& _info) {
        if (m_skip_traps.load(std::memory_order_relaxed)) return true;
        std::string args;
        std::string label;
        if (_info.info.is<alx::varmap>()) {
            const alx::varmap& m = _info.info.to<alx::varmap>();
            if (m.contain("args")) {
                const alx::variant& a = m.value("args");
                args = core::utils::repr(a);
                label = a.is<std::string>() ? a.to<std::string>() : short_label(args);
            }
        }
        return stop_wave(_info, m_main, "trap", std::move(label), std::move(args));
    }

    bool hook::stop_wave(alx::script::hook_info& _info, thread_ctx& _ctx, const char* _reason,
                         std::string _label, std::string _trap_args) {
        proto::stop_info si;
        si.reason = _reason;
        si.frames = frames(_info, _info.wkdt_row(), _info.wkdt_col(), _ctx.id != 0);
        bool first = false;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (!m_run_active.load() || m_exit.load()) return true;
            if (!m_stopped.load()) {
                if (std::strcmp(_reason, "sync") == 0) return true; // the joined wave already resumed
                m_stopped.store(true);
                ++m_stop_seq;
                m_trigger = _ctx.id;
                first = true;
            }
            si.thread = _ctx.id;
            si.parent = _ctx.parent;
            si.trigger = m_trigger;
            si.stop = m_stop_seq;
            si.known = m_children.size() + 1;
            if (_ctx.id != m_trigger) {
                si.reason = "sync";
                _label.clear();
                _trap_args.clear();
            }
            if (first) {
                m_ctl.set_debug_active(true);
                m_ctl.clear_debug();
            }
        }
        si.label = std::move(_label);
        si.trap_args = std::move(_trap_args);
        m_out.send_event(proto::make_stopped(si));
        return park(_info, _ctx);
    }

    bool hook::park(alx::script::hook_info& _info, thread_ctx& _ctx) {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait_for(lk, std::chrono::milliseconds(10), [this] {
                    return !m_stopped.load() || m_exit.load() || m_ctl.stop_requested();
                });
                if (m_exit.load() || m_ctl.stop_requested()) return false;
                if (!m_stopped.load()) return true;
            }
            proto::message cmd;
            if (!m_ctl.pop_debug_for(_ctx.id, cmd, 50)) continue;
            if (cmd.type == proto::kContinue) {
                resume_all();
                return true;
            }
            if (cmd.type == proto::kStep) {
                if (_ctx.id != 0) continue;
                const std::string kind = parse_step(cmd.payload);
                m_step = kind == "in" ? step_mode::in : kind == "out" ? step_mode::out
                                                                      : step_mode::over;
                m_step_depth = func_depth(_info);
                m_step_epoch = resume_all();
                return true;
            }
            if (cmd.type == proto::kSnapshot) {
                serve_snapshot(_info, cmd);
                continue;
            }
            if (cmd.type == proto::kView) {
                serve_view(_info, cmd);
                continue;
            }
        }
    }

    alx::uint_64 hook::resume_all() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_stopped.store(false);
            m_trigger = 0;
            m_ctl.clear_debug();
            m_ctl.set_debug_active(false);
        }
        const alx::uint_64 epoch = ++m_epoch;
        m_cv.notify_all();
        return epoch;
    }

    void hook::observe(bool _attach, alx::script::engine* _child, std::thread::id _parent,
                       std::thread::id _child_tid) {
        if (!_child) return;
        if (_attach) {
            auto* ctx = new thread_ctx;
            ctx->owner = this;
            ctx->tid = _child_tid;
            bool orphan = false;
            {
                std::lock_guard<std::mutex> lk(m_mtx);
                ctx->id = m_next_id++;
                const auto p = m_tids.find(_parent);
                orphan = p == m_tids.end();
                ctx->parent = orphan ? 0 : static_cast<alx::int_64>(p->second);
                m_children[ctx->id] = ctx;
                m_tids[_child_tid] = ctx->id;
            }
            _child->set_hook(&hook::entry, ctx, 0);
            if (orphan)
                m_out.send_event(proto::make_log(
                    "warn", "thread parent exited before attach; attached to the main thread"));
            return;
        }
        thread_ctx* ctx = nullptr;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            const auto t = m_tids.find(_child_tid);
            if (t != m_tids.end()) {
                const auto c = m_children.find(t->second);
                if (c != m_children.end()) {
                    ctx = c->second;
                    m_children.erase(c);
                }
                m_tids.erase(t);
            }
        }
        if (ctx) {
            m_src.clear_cache();
            delete ctx;
        }
        m_cv.notify_all();
    }

    void hook::serve_snapshot(alx::script::hook_info& _info, const proto::message& _cmd) {
        const std::string ref = text_field(_cmd.payload, "ref");
        const alx::uint_64 limit =
            static_cast<alx::uint_64>(int_field(_cmd.payload, "limit"));
        const alx::uint_64 ofst =
            static_cast<alx::uint_64>(int_field(_cmd.payload, "ofst"));
        const alx::uint_64 thread =
            static_cast<alx::uint_64>(int_field(_cmd.payload, "thread"));
        std::vector<proto::snapshot_node> nodes;
        bool more = false;
        if (!snapshot::serve(_info, ref, ofst, limit, m_src.embed_owner(), nodes, more)) {
            m_out.send_event(proto::make_log("warn", "snapshot ref does not resolve: " + ref));
            return;
        }
        m_out.send_event(proto::make_snapshot(ref, ofst, thread, nodes, more));
    }

    void hook::serve_view(alx::script::hook_info& _info, const proto::message& _cmd) {
        const std::string ref = text_field(_cmd.payload, "ref");
        const alx::uint_64 thread =
            static_cast<alx::uint_64>(int_field(_cmd.payload, "thread"));
        const alx::variant* v = nullptr;
        std::string err;
        alx::variant view;
        if (!snapshot::resolve(_info, ref, v, err)) {
            m_out.send_event(proto::make_view(ref, false, err, std::move(view), thread, _cmd.id), true);
            return;
        }
        if (!v || !v->is<alx::anyptr>()) {
            m_out.send_event(
                proto::make_view(ref, false, "view requires a handle: " + ref, std::move(view), thread,
                                 _cmd.id),
                true);
            return;
        }
        bool ok = false;
        try {
            ok = _info.wkview(v->to<alx::anyptr>(), view);
        } catch (const std::exception& _e) {
            err = _e.what();
        } catch (...) {
            err = "view provider failed";
        }
        if (!ok && err.empty()) ok = core::inspect::view(v->to<alx::anyptr>(), view);
        if (!ok && err.empty()) err = "no view registered for this handle";
        m_out.send_event(proto::make_view(ref, ok, err, std::move(view), thread, _cmd.id), true);
    }

    void hook::apply_pending_breakpoints() {
        proto::message cmd;
        while (m_ctl.take_breakpoints(cmd)) apply_breakpoints(cmd);
    }

    void hook::apply_breakpoints(const proto::message& _cmd) {
        std::string source;
        std::vector<proto::breakpoint> items;
        if (!proto::parse_set_breakpoints(_cmd.payload, source, items)) return;
        m_bps.erase(std::remove_if(m_bps.begin(), m_bps.end(),
                                   [&source](const proto::breakpoint& _b) { return _b.source == source; }),
                    m_bps.end());
        m_bps.insert(m_bps.end(), items.begin(), items.end());
        m_bp_views.clear();
    }

    const void* hook::current_entity(alx::script::hook_info& _info) const {
        const alx::uint_64 depth = _info.wkfm_size();
        if (!depth) return _info.wken_root();
        return _info.wkfm_eptr(depth - 1);
    }

    const proto::breakpoint* hook::breakpoint_at(alx::script::hook_info& _info, const void* _entity,
                                                 alx::uint_64 _line) {
        auto it = m_bp_views.find(_entity);
        if (it == m_bp_views.end()) {
            bp_view view;
            view.source = m_src.of(_info, _entity).id;
            for (const auto& b : m_bps)
                if (b.enabled && b.source == view.source && b.line) view.lines.emplace(b.line, b);
            it = m_bp_views.emplace(_entity, std::move(view)).first;
        }
        const auto found = it->second.lines.find(_line);
        return found == it->second.lines.end() ? nullptr : &found->second;
    }

    alx::uint_64 hook::func_depth(alx::script::hook_info& _info) const {
        alx::uint_64 depth = 0;
        for (alx::uint_64 i = 0; i < _info.wkfm_size(); ++i)
            if (_info.wkfm_func(i) != "?") ++depth;
        return depth;
    }

    std::vector<proto::stack_frame> hook::frames(alx::script::hook_info& _info, alx::uint_64 _row,
                                                 alx::uint_64 _col, bool _child) {
        std::vector<proto::stack_frame> out;
        const void* root = _info.wken_root();
        const std::list<alx::varmap> chain = _info.wkdt_fpos();
        alx::uint_64 wi = _info.wkfm_size();
        bool first = true;
        for (auto it = chain.begin(); it != chain.end(); ++it) {
            const alx::varmap& m = *it;
            proto::stack_frame f;
            f.line = static_cast<alx::uint_64>(int_field(m, "row"));
            f.col = static_cast<alx::uint_64>(int_field(m, "col"));
            f.ofst = static_cast<alx::uint_64>(int_field(m, "ofst"));
            f.func = text_field(m, "func");
            if (std::next(it) == chain.end()) {
                f.ref = "root";
                if (!_child) f.source = m_src.of(_info, root).id;
            } else {
                bool found = false;
                alx::uint_64 idx = 0;
                if (wi > 0) {
                    alx::uint_64 j = wi;
                    while (j > 0) {
                        --j;
                        if (_info.wkfm_func(j) == f.func) {
                            idx = j;
                            found = true;
                            break;
                        }
                    }
                }
                if (found) {
                    wi = idx;
                    f.ref = "f" + std::to_string(idx);
                    const void* e = _info.wkfm_eptr(idx);
                    if (!_child || e != root) f.source = m_src.of(_info, e).id;
                    f.alias = _info.wken_name(e);
                } else {
                    f.internal = true; // no activation matched: show it as an internal frame
                }
            }
            if (first) {
                first = false;
                if (!f.line) {
                    f.line = _row;
                    f.col = _col;
                }
            }
            out.push_back(std::move(f));
        }
        if (out.empty()) { // defensive: fpos always ends with the top-level entry
            proto::stack_frame top;
            top.ref = "root";
            if (!_child) top.source = m_src.of(_info, root).id;
            top.line = _row;
            top.col = _col;
            out.push_back(std::move(top));
        }
        return out;
    }
} // namespace runner
