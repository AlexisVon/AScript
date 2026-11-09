#ifndef ARUNNER_SOURCE_H
#define ARUNNER_SOURCE_H

#include <ascript.h>

#include <mutex>
#include <string>
#include <unordered_map>

// source_registry — stable source identity per engine entity, resolved path included
namespace runner {
    class source_registry {
    public:
        // entry — identity and path of one entity
        struct entry {
            std::string id; // e.g. "file:/abs/a.axc", or the root's own id
            std::string path; // canonical path; empty for an inline root
        };

        // set_root — the executed script's own identity, from the exec request
        void set_root(std::string _id, std::string _path);
        // embed_owner — the root .axp that owns embedded ("@key") modules; empty otherwise
        std::string embed_owner() const;
        // clear_cache — drop entity identities; a dead child engine frees the pointers
        void clear_cache();

        // of — identity of _entity, computed once per entity; any engine thread may call
        entry of(alx::script::hook_info& _info, const void* _entity);

    private:
        mutable std::mutex m_mtx;
        std::string m_root_id;
        std::string m_root_path;
        std::string m_embed_owner;
        std::unordered_map<const void*, entry> m_cache;
    };
} // namespace runner

#endif // ARUNNER_SOURCE_H
