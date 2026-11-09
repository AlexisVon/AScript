#ifndef AEXT_VEC_H
#define AEXT_VEC_H

#include "../extend.h"

// $vec_xxx — vec container operations. Containers are passed directly as
// fwrap views (variant&, zero-copy into caller slots); mutations in place.
namespace core {
    class vec : public extend {
    public:
        static const vec& instance() {
            static vec inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_VEC_H
