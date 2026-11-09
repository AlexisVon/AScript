#ifndef AEXT_SYS_H
#define AEXT_SYS_H

#include "../extend.h"

// $sys_xxx extensions: $sys_exec / $sys_datetime / $sys_sleep
namespace core {
    class sys : public extend {
    public:
        static const sys& instance() {
            static sys inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_SYS_H
