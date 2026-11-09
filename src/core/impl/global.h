#ifndef AEXT_GLOBAL_H
#define AEXT_GLOBAL_H

#include "../extend.h"

#define STRIFY_(x) #x
#define STRIFY(x) STRIFY_(x)

#define ENGINE_ETYPE "axc-1.0"
#define ENGINE_VTYPE 0
#define ENGINE_VERSION ENGINE_ETYPE "." STRIFY(ENGINE_VTYPE)

// $etype / $vtype — engine-level defines
namespace core {
    class global : public extend {
    public:
        static const global& instance() {
            static global inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_GLOBAL_H
