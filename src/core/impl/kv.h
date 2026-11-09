#ifndef AEXT_KV_H
#define AEXT_KV_H

#include "../extend.h"

// $kv_xxx extensions: cross-engine shared key-value store (literal-only values)
namespace core {
    class kv : public extend {
    public:
        static const kv& instance() {
            static kv inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_KV_H
