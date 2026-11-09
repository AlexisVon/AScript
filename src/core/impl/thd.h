#ifndef AEXT_THD_H
#define AEXT_THD_H

#include "../core.h"
#include "../extend.h"

#include <atomic>
#include <thread>

// $thd_xxx extensions: thread execution with isolated engine instances
namespace core {

    // thd_handle — a child engine's handle (RAII: interrupted, joined and destroyed on drop)
    struct thd_handle : alx::noncopyable {
        std::thread thr;
        alx::script::engine::result res;
        alx::script::engine* eng = nullptr;
        std::atomic<bool> done{false}; // worker finished; res is safe to read

        ~thd_handle() {
            if (eng && eng->is_running()) eng->set_interrupt();
            if (thr.joinable()) thr.join();
            if (eng) core::destroy(eng);
        }
    };

    class thd : public extend {
    public:
        static const thd& instance() {
            static thd inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_THD_H
