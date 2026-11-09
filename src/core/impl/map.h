#ifndef AEXT_MAP_H
#define AEXT_MAP_H

#include "../extend.h"

// $map_xxx — map container operations. Containers are passed directly as
// fwrap views (variant&, zero-copy into caller slots); mutations in place.
namespace core {
    class map : public extend {
    public:
        static const map& instance() {
            static map inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_MAP_H
