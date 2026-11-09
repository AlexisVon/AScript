#include "source_registry.h"

#include <filesystem>

namespace runner {
    namespace {
        // canonical — weakly canonical path, or the input when that fails
        std::string canonical(const std::string& _path) {
            if (_path.empty()) return std::string();
            std::error_code ec;
            const std::filesystem::path p = std::filesystem::weakly_canonical(_path, ec);
            return ec ? _path : p.string();
        }
    } // namespace

    void source_registry::set_root(std::string _id, std::string _path) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_root_id = std::move(_id);
        m_root_path = canonical(_path);
        const bool axp = m_root_path.size() > 4 &&
                         m_root_path.compare(m_root_path.size() - 4, 4, ".axp") == 0;
        m_embed_owner = axp ? m_root_path : std::string();
        m_cache.clear();
    }

    std::string source_registry::embed_owner() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_embed_owner;
    }

    void source_registry::clear_cache() {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_cache.clear();
    }

    source_registry::entry source_registry::of(alx::script::hook_info& _info, const void* _entity) {
        std::lock_guard<std::mutex> lk(m_mtx);
        const auto cached = m_cache.find(_entity);
        if (cached != m_cache.end()) return cached->second;

        entry e;
        if (!_entity || _entity == _info.wken_root()) {
            e.id = m_root_id;
            e.path = m_root_path;
        } else {
            e.path = canonical(_info.wken_path(_entity));
            if (e.path.empty()) e.path = _info.wken_file(_entity);
            if (!e.path.empty() && e.path[0] == '@') // embedded module: the GUI opens "owner.axp@key"
                e.id = m_embed_owner.empty() ? "axp:" + e.path : "file:" + m_embed_owner + e.path;
            else if (!e.path.empty())
                e.id = "file:" + e.path;
            else
                e.id = "unknown";
        }
        return m_cache.emplace(_entity, std::move(e)).first->second;
    }
} // namespace runner
