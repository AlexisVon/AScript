#include "inspect.h"

#include <mutex>
#include <unordered_map>

namespace core {
    namespace inspect {
        namespace {
            struct entry {
                const char* label = nullptr;
                describe_fn describe = nullptr;
                view_fn view = nullptr;
            };

            std::unordered_map<alx::uint_64, entry>& table() {
                static std::unordered_map<alx::uint_64, entry> t;
                return t;
            }

            std::mutex& table_mtx() {
                static std::mutex m;
                return m;
            }

            child_fn& observer_fn() {
                static child_fn fn = nullptr;
                return fn;
            }

            void*& observer_ud() {
                static void* ud = nullptr;
                return ud;
            }

            std::mutex& observer_mtx() {
                static std::mutex m;
                return m;
            }

            bool lookup(const alx::anyptr& _p, entry& _out) {
                const alx::uint_64 rid = _p.rid();
                if (!rid) return false;
                std::lock_guard<std::mutex> lk(table_mtx());
                const auto it = table().find(rid);
                if (it == table().end()) return false;
                _out = it->second;
                return true;
            }
        } // namespace

        void add(alx::uint_64 _rid, const char* _name, describe_fn _describe, view_fn _view) {
            if (!_rid || !_name || !_name[0]) return;
            std::lock_guard<std::mutex> lk(table_mtx());
            table()[_rid] = entry{_name, _describe, _view};
        }

        const char* name(const alx::anyptr& _p) {
            entry e;
            return lookup(_p, e) ? e.label : nullptr;
        }

        bool describe(const alx::anyptr& _p, std::string& _out) {
            entry e;
            if (!lookup(_p, e) || !e.describe) return false;
            try {
                _out = e.describe(_p);
            } catch (...) {
                return false;
            }
            return !_out.empty();
        }

        bool view(const alx::anyptr& _p, alx::variant& _out) {
            entry e;
            if (!lookup(_p, e) || !e.view) return false;
            try {
                _out = e.view(_p);
            } catch (...) {
                return false;
            }
            return !_out.null();
        }

        void set_child_observer(child_fn _fn, void* _ud) {
            std::lock_guard<std::mutex> lk(observer_mtx());
            observer_fn() = _fn;
            observer_ud() = _ud;
        }

        void notify_child(bool _attach, alx::script::engine* _child, std::thread::id _parent,
                          std::thread::id _child_tid) {
            child_fn fn = nullptr;
            void* ud = nullptr;
            {
                std::lock_guard<std::mutex> lk(observer_mtx());
                fn = observer_fn();
                ud = observer_ud();
            }
            if (fn) fn(ud, _attach, _child, _parent, _child_tid);
        }
    } // namespace inspect
} // namespace core
