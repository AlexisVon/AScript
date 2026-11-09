#ifndef AEXT_LST_H
#define AEXT_LST_H

#include "../extend.h"

// $lst_xxx — lst container operations. Containers are passed directly as
// fwrap views (variant&, zero-copy into caller slots); mutations in place.
namespace core {
    class lst : public extend {
    public:
        static const lst& instance() {
            static lst inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_LST_H
