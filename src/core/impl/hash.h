#ifndef AEXT_HASH_H
#define AEXT_HASH_H

#include "../extend.h"

// $hash_xxx extensions: hashing and checksum
namespace core {
    class hash : public extend {
    public:
        static const hash& instance() {
            static hash inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_HASH_H
