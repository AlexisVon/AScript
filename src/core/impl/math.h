#ifndef AEXT_MATH_H
#define AEXT_MATH_H

#include "../extend.h"

// $math_xxx — static definitions and math extensions ($math_PI ...)
namespace core {
    class math : public extend {
    public:
        static const math& instance() {
            static math inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_MATH_H
