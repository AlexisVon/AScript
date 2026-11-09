#include "map.h"

// $map_xxx — map container operations (direct views into caller slots)
namespace core {
    namespace map_impl {

        using utils::as_map;
        using utils::as_str;

        static void ext_keys(alx::script::fwrap& fw) {
            const auto& m = as_map(fw[0], "map_keys map");
            alx::varvec result;
            for (auto it = m.begin(); it != m.end(); ++it) result.push_back(alx::variant(it.key()));
            fw.freturn(alx::variant(std::move(result)));
        }

        static void ext_values(alx::script::fwrap& fw) {
            const auto& m = as_map(fw[0], "map_values map");
            alx::varvec result;
            for (auto it = m.begin(); it != m.end(); ++it) result.push_back(it.value());
            fw.freturn(alx::variant(std::move(result)));
        }

        static void ext_has(alx::script::fwrap& fw) {
            const auto& m = as_map(fw[0], "map_has map");
            fw.freturn(alx::variant(m.contain(as_str(fw[1], "map_has key"))));
        }

        static void ext_get(alx::script::fwrap& fw) {
            const auto& m = as_map(fw[0], "map_get map");
            const auto& k = as_str(fw[1], "map_get key");
            if (m.contain(k)) {
                fw.freturn(m.value(k));
            } else if (fw.size() > 2) {
                fw.freturn(fw[2]); // default
            } else {
                fw.freturn(); // no default -> null
            }
        }

        static void ext_size(alx::script::fwrap& fw) {
            const auto& m = as_map(fw[0], "map_size map");
            fw.freturn(alx::variant(static_cast<alx::int_64>(m.size())));
        }

        static void ext_insert(alx::script::fwrap& fw) {
            auto& m = as_map(fw[0], "map_insert map");
            const auto& k = as_str(fw[1], "map_insert key");
            if (fw.size() < 3)
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "map_insert: expected a value"};
            m[k] = fw[2];
            fw.freturn();
        }

        static void ext_remove(alx::script::fwrap& fw) {
            auto& m = as_map(fw[0], "map_remove map");
            const auto& k = as_str(fw[1], "map_remove key");
            if (m.contain(k)) m.erase(k);
            fw.freturn();
        }

        static void ext_merge(alx::script::fwrap& fw) {
            auto& m = as_map(fw[0], "map_merge map");
            alx::variant& other = fw[1];
            if (!other.is<alx::varmap>())
                throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                    "map_merge other: expected map"};
            if (&other == &fw[0]) fw.freturn(); // self-merge is a no-op
            const auto& s = other.to<alx::varmap>();
            for (auto it = s.begin(); it != s.end(); ++it) m[it.key()] = it.value();
            fw.freturn();
        }

    } // namespace map_impl

    void map::install(alx::script::engine* _e) const {
        _e->set_extend("map_keys", map_impl::ext_keys);
        _e->set_extend("map_values", map_impl::ext_values);
        _e->set_extend("map_has", map_impl::ext_has);
        _e->set_extend("map_get", map_impl::ext_get);
        _e->set_extend("map_size", map_impl::ext_size);
        _e->set_extend("map_insert", map_impl::ext_insert);
        _e->set_extend("map_remove", map_impl::ext_remove);
        _e->set_extend("map_merge", map_impl::ext_merge);
    }

    const char* map::doc_path() const {
        return "map.json";
    }

} // namespace core
