#ifndef AEXT_BYTES_H
#define AEXT_BYTES_H

#include "../extend.h"

// $bytes_xxx extensions: binary data operations
namespace core {
    class bytes : public extend {
    public:
        static const bytes& instance() {
            static bytes inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_BYTES_H
