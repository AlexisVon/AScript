#ifndef AEXT_FS_H
#define AEXT_FS_H

#include "../extend.h"

// $fs_xxx — file/dir operations (alexis afile). info -> map, child -> vec<map>;
// create/rmfile/mkdir/rmdir return bool (false on failure, no throw).
namespace core {
    class fs : public extend {
    public:
        static const fs& instance() {
            static fs inst;
            return inst;
        }
        void install(alx::script::engine* _e) const override;

    protected:
        const char* doc_path() const override;
    };
} // namespace core

#endif // AEXT_FS_H
