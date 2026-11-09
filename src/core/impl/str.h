#ifndef AEXT_STR_H
#define AEXT_STR_H

#include "../extend.h"

// $str_xxx extensions: string operations
namespace core {
    class str : public extend {
    public:
        static const str& instance() {
            static str inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_STR_H
