#ifndef AEXT_REGX_H
#define AEXT_REGX_H

#include "../extend.h"

// $regx_xxx extensions: regex operations
namespace core {
    class regx : public extend {
    public:
        static const regx& instance() {
            static regx inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_REGX_H
