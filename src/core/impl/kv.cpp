#include "kv.h"

#include <map>
#include <shared_mutex>

// $kv_xxx extensions — cross-engine shared key-value store (literal-only values)
namespace core {
    namespace kv_impl {

        // per-entry: value + own rw lock
        struct kv_slot {
            alx::variant value;
            std::shared_mutex mtx;
        };

        // global pool: std::map guarantees reference stability
        static std::map<std::string, kv_slot> s_pool;
        static std::shared_mutex s_pool_mtx;

        // --- extensions ---

        static void ext_store(alx::script::fwrap& fw) {
            std::string key = core::utils::as_str(fw[0], "kv_store key");
            core::utils::assert_literal(fw[1], "kv_store value");

            // pool R → find
            {
                std::shared_lock pool_r(s_pool_mtx);
                auto it = s_pool.find(key);
                if (it != s_pool.end()) {
                    std::unique_lock slot_w(it->second.mtx);
                    it->second.value = fw[1];
                    return;
                }
            }
            // not found → pool W → double-check → create
            {
                std::unique_lock pool_w(s_pool_mtx);
                auto it = s_pool.find(key);
                if (it == s_pool.end()) {
                    auto [ins, _] = s_pool.emplace(std::piecewise_construct,
                                                   std::forward_as_tuple(key),
                                                   std::forward_as_tuple());
                    ins->second.value = fw[1];
                } else {
                    std::unique_lock slot_w(it->second.mtx);
                    it->second.value = fw[1];
                }
            }
        }

        static void ext_load(alx::script::fwrap& fw) {
            std::string key = core::utils::as_str(fw[0], "kv_load key");
            std::shared_lock pool_r(s_pool_mtx);
            auto it = s_pool.find(key);
            if (it != s_pool.end()) {
                std::shared_lock slot_r(it->second.mtx);
                fw.freturn(it->second.value); // copy
            }
            // not found → return void
        }

        static void ext_has(alx::script::fwrap& fw) {
            std::string key = core::utils::as_str(fw[0], "kv_has key");
            std::shared_lock pool_r(s_pool_mtx);
            fw.freturn(alx::variant(s_pool.find(key) != s_pool.end()));
        }

        static void ext_rm(alx::script::fwrap& fw) {
            std::string key = core::utils::as_str(fw[0], "kv_rm key");
            std::unique_lock pool_w(s_pool_mtx);
            auto it = s_pool.find(key);
            if (it == s_pool.end()) {
                fw.freturn(alx::variant(false));
                return;
            }
            std::unique_lock slot_w(it->second.mtx);
            s_pool.erase(it);
            fw.freturn(alx::variant(true));
        }

        // cas(key, expect, newval) → bool
        // void expect: matches non-existence OR stored void (put-if-absent)
        static void ext_cas(alx::script::fwrap& fw) {
            std::string key = core::utils::as_str(fw[0], "kv_cas key");
            const auto& expect = fw[1];
            core::utils::assert_literal(fw[2], "kv_cas");

            // pool R → find → decide in one atomic window
            {
                std::shared_lock pool_r(s_pool_mtx);
                auto it = s_pool.find(key);
                if (it != s_pool.end()) {
                    // found → slot W → compare → maybe swap
                    std::unique_lock slot_w(it->second.mtx);
                    bool match = expect.null() || (it->second.value == expect);
                    if (match) {
                        it->second.value = fw[2];
                        fw.freturn(alx::variant(true));
                    } else {
                        fw.freturn(alx::variant(false));
                    }
                    return;
                }
                // not found → only proceed if expect is null
                if (!expect.null()) {
                    fw.freturn(alx::variant(false));
                    return;
                }
            }
            // pool W upgrade → double-check → create
            {
                std::unique_lock pool_w(s_pool_mtx);
                auto it = s_pool.find(key);
                if (it != s_pool.end()) {
                    std::unique_lock slot_w(it->second.mtx);
                    if (it->second.value.null()) {
                        it->second.value = fw[2];
                        fw.freturn(alx::variant(true));
                    } else {
                        fw.freturn(alx::variant(false));
                    }
                } else {
                    auto [ins, _] = s_pool.emplace(std::piecewise_construct,
                                                   std::forward_as_tuple(key),
                                                   std::forward_as_tuple());
                    ins->second.value = fw[2];
                    fw.freturn(alx::variant(true));
                }
            }
        }

        static void ext_clean(alx::script::fwrap&) {
            std::unique_lock pool_w(s_pool_mtx);
            for (auto it = s_pool.begin(); it != s_pool.end();) {
                if (it->second.value.null())
                    it = s_pool.erase(it);
                else
                    ++it;
            }
        }

        static void ext_clear(alx::script::fwrap&) {
            std::unique_lock pool_w(s_pool_mtx);
            s_pool.clear();
        }

    } // namespace kv_impl

    void kv::install(alx::script::engine* _e) const {
        _e->set_extend("kv_store", kv_impl::ext_store);
        _e->set_extend("kv_load", kv_impl::ext_load);
        _e->set_extend("kv_has", kv_impl::ext_has);
        _e->set_extend("kv_rm", kv_impl::ext_rm);
        _e->set_extend("kv_cas", kv_impl::ext_cas);
        _e->set_extend("kv_clean", kv_impl::ext_clean);
        _e->set_extend("kv_clear", kv_impl::ext_clear);
    }

    const char* kv::doc_path() const {
        return "kv.json";
    }

} // namespace core
