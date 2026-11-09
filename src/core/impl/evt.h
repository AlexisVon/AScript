#ifndef AEXT_EVT_H
#define AEXT_EVT_H

#include "../extend.h"

// $evt_xxx extensions: event delivery into the script's yield() service points
namespace core {
    class evt : public extend {
    public:
        static const evt& instance() {
            static evt inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_EVT_H
