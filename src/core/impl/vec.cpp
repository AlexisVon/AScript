#include "vec.h"

#include <algorithm>

// $vec_xxx — vec container operations (direct views into caller slots)
namespace core {
    namespace vec_impl {

        using utils::as_vec;
        using utils::as_int;

        // veq — same-type equality (int/float/string/bool)
        static bool veq(const alx::variant& _a, const alx::variant& _b) {
            if (_a.is<alx::int_64>() && _b.is<alx::int_64>()) return _a.to<alx::int_64>() == _b.to<alx::int_64>();
            if (_a.is<double>() && _b.is<double>()) return _a.to<double>() == _b.to<double>();
            if (_a.is<std::string>() && _b.is<std::string>()) return _a.to<std::string>() == _b.to<std::string>();
            if (_a.is<bool>() && _b.is<bool>()) return _a.to<bool>() == _b.to<bool>();
            return false;
        }

        static void ext_size(alx::script::fwrap& fw) {
            const auto& v = as_vec(fw[0], "vec_size vec");
            fw.freturn(alx::variant(static_cast<alx::int_64>(v.size())));
        }

        static void ext_find(alx::script::fwrap& fw) {
            if (fw.size() < 2)
                throw alx::script::script_exception{alx::script::error_type::ArgError, "vec_find: expected a value"};
            const auto& v = as_vec(fw[0], "vec_find vec");
            for (size_t i = 0; i < v.size(); i++)
                if (veq(v[i], fw[1])) {
                    fw.freturn(alx::variant(static_cast<alx::int_64>(i)));
                    return;
                }
            fw.freturn(alx::variant(static_cast<alx::int_64>(-1)));
        }

        static void ext_has(alx::script::fwrap& fw) {
            if (fw.size() < 2)
                throw alx::script::script_exception{alx::script::error_type::ArgError, "vec_has: expected a value"};
            const auto& v = as_vec(fw[0], "vec_has vec");
            for (const auto& e : v)
                if (veq(e, fw[1])) {
                    fw.freturn(alx::variant(true));
                    return;
                }
            fw.freturn(alx::variant(false));
        }

        static void ext_insert(alx::script::fwrap& fw) {
            if (fw.size() < 3)
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "vec_insert: expected position and value"};
            auto& v = as_vec(fw[0], "vec_insert vec");
            alx::int_64 p = as_int(fw[1], "vec_insert pos");
            if (p < 0) p = 0;
            if (p > static_cast<alx::int_64>(v.size())) p = static_cast<alx::int_64>(v.size());
            v.insert(v.begin() + static_cast<size_t>(p), fw[2]);
            fw.freturn();
        }

        static void ext_remove(alx::script::fwrap& fw) {
            if (fw.size() < 2)
                throw alx::script::script_exception{alx::script::error_type::ArgError, "vec_remove: expected position"};
            auto& v = as_vec(fw[0], "vec_remove vec");
            alx::int_64 idx = as_int(fw[1], "vec_remove pos");
            if (idx >= 0 && idx < static_cast<alx::int_64>(v.size())) v.erase(v.begin() + static_cast<size_t>(idx));
            fw.freturn();
        }

        static void ext_sort(alx::script::fwrap& fw) {
            auto& v = as_vec(fw[0], "vec_sort vec");
            bool desc = fw.size() > 1 && fw[1].is<bool>() && fw[1].to<bool>();
            std::sort(v.begin(), v.end(), [desc](const alx::variant& x, const alx::variant& y) {
                if (x.is<alx::int_64>() && y.is<alx::int_64>())
                    return desc ? x.to<alx::int_64>() > y.to<alx::int_64>() : x.to<alx::int_64>() < y.to<alx::int_64>();
                if (x.is<double>() && y.is<double>())
                    return desc ? x.to<double>() > y.to<double>() : x.to<double>() < y.to<double>();
                if (x.is<std::string>() && y.is<std::string>())
                    return desc ? x.to<std::string>() > y.to<std::string>() : x.to<std::string>() < y.to<std::string>();
                return false;
            });
            fw.freturn();
        }

        static void ext_reverse(alx::script::fwrap& fw) {
            auto& v = as_vec(fw[0], "vec_reverse vec");
            std::reverse(v.begin(), v.end());
            fw.freturn();
        }

        static void ext_merge(alx::script::fwrap& fw) {
            auto& v = as_vec(fw[0], "vec_merge vec");
            alx::variant& other = fw[1];
            if (&other == &fw[0]) { // self-merge — snapshot, insert realloc invalidates iterators
                alx::varvec tmp = v;
                v.insert(v.end(), tmp.begin(), tmp.end());
            } else if (other.is<alx::varvec>()) {
                const auto& o = other.to<alx::varvec>();
                v.insert(v.end(), o.begin(), o.end());
            } else if (other.is<alx::varlst>()) {
                for (const auto& e : other.to<alx::varlst>()) v.push_back(e);
            } else {
                throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                    "vec_merge other: expected vec or lst"};
            }
            fw.freturn();
        }

    } // namespace vec_impl

    void vec::install(alx::script::engine* _e) const {
        _e->set_extend("vec_size", vec_impl::ext_size);
        _e->set_extend("vec_find", vec_impl::ext_find);
        _e->set_extend("vec_has", vec_impl::ext_has);
        _e->set_extend("vec_insert", vec_impl::ext_insert);
        _e->set_extend("vec_remove", vec_impl::ext_remove);
        _e->set_extend("vec_sort", vec_impl::ext_sort);
        _e->set_extend("vec_reverse", vec_impl::ext_reverse);
        _e->set_extend("vec_merge", vec_impl::ext_merge);
    }

    const char* vec::doc_path() const {
        return "vec.json";
    }

} // namespace core
