#include "lst.h"

// $lst_xxx — lst container operations (direct views into caller slots)
namespace core {
    namespace lst_impl {

        using utils::as_lst;
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
            const auto& l = as_lst(fw[0], "lst_size lst");
            fw.freturn(alx::variant(static_cast<alx::int_64>(l.size())));
        }

        static void ext_find(alx::script::fwrap& fw) {
            if (fw.size() < 2)
                throw alx::script::script_exception{alx::script::error_type::ArgError, "lst_find: expected a value"};
            const auto& l = as_lst(fw[0], "lst_find lst");
            size_t i = 0;
            for (const auto& e : l) {
                if (veq(e, fw[1])) {
                    fw.freturn(alx::variant(static_cast<alx::int_64>(i)));
                    return;
                }
                i++;
            }
            fw.freturn(alx::variant(static_cast<alx::int_64>(-1)));
        }

        static void ext_has(alx::script::fwrap& fw) {
            if (fw.size() < 2)
                throw alx::script::script_exception{alx::script::error_type::ArgError, "lst_has: expected a value"};
            const auto& l = as_lst(fw[0], "lst_has lst");
            for (const auto& e : l)
                if (veq(e, fw[1])) {
                    fw.freturn(alx::variant(true));
                    return;
                }
            fw.freturn(alx::variant(false));
        }

        static void ext_insert(alx::script::fwrap& fw) {
            if (fw.size() < 3)
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "lst_insert: expected position and value"};
            auto& l = as_lst(fw[0], "lst_insert lst");
            alx::int_64 p = as_int(fw[1], "lst_insert pos");
            if (p < 0) p = 0;
            if (p > static_cast<alx::int_64>(l.size())) p = static_cast<alx::int_64>(l.size());
            auto it = l.begin();
            std::advance(it, static_cast<size_t>(p));
            l.insert(it, fw[2]);
            fw.freturn();
        }

        static void ext_remove(alx::script::fwrap& fw) {
            if (fw.size() < 2)
                throw alx::script::script_exception{alx::script::error_type::ArgError, "lst_remove: expected position"};
            auto& l = as_lst(fw[0], "lst_remove lst");
            alx::int_64 idx = as_int(fw[1], "lst_remove pos");
            if (idx >= 0 && idx < static_cast<alx::int_64>(l.size())) {
                auto it = l.begin();
                std::advance(it, static_cast<size_t>(idx));
                l.erase(it);
            }
            fw.freturn();
        }

        static void ext_merge(alx::script::fwrap& fw) {
            auto& l = as_lst(fw[0], "lst_merge lst");
            alx::variant& other = fw[1];
            if (&other == &fw[0]) { // self-merge — snapshot, push-while-iterating is unsafe
                alx::varlst tmp = l;
                for (const auto& e : tmp) l.push_back(e);
            } else if (other.is<alx::varlst>()) {
                for (const auto& e : other.to<alx::varlst>()) l.push_back(e);
            } else if (other.is<alx::varvec>()) {
                for (const auto& e : other.to<alx::varvec>()) l.push_back(e);
            } else {
                throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                    "lst_merge other: expected lst or vec"};
            }
            fw.freturn();
        }

    } // namespace lst_impl

    void lst::install(alx::script::engine* _e) const {
        _e->set_extend("lst_size", lst_impl::ext_size);
        _e->set_extend("lst_find", lst_impl::ext_find);
        _e->set_extend("lst_has", lst_impl::ext_has);
        _e->set_extend("lst_insert", lst_impl::ext_insert);
        _e->set_extend("lst_remove", lst_impl::ext_remove);
        _e->set_extend("lst_merge", lst_impl::ext_merge);
    }

    const char* lst::doc_path() const {
        return "lst.json";
    }

} // namespace core
