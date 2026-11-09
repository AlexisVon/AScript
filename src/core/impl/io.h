#ifndef AEXT_IO_H
#define AEXT_IO_H

#include "../extend.h"

// $io_xxx extensions: $io_print / $io_input / $io_fread / $io_fwrite / $io_tojs / $io_fmjs
namespace core {
    class io : public extend {
    public:
        static const io& instance() {
            static io inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_IO_H
