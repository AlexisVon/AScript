#include "extend.h"
#include "inspect.h"
#include "res.h"
#include "aregex_ex.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <shared_mutex>

namespace core {

    const alx::json_object& extend::docs() const {
        static std::map<std::string, alx::json_object> cache;
        static std::shared_mutex mtx;
        const char* res_key = doc_path();
        { // read lock — fast path for cached entries
            std::shared_lock rlk(mtx);
            auto it = cache.find(res_key);
            if (it != cache.end()) return it->second;
        }
        { // write lock — double-check before loading
            std::unique_lock wlk(mtx);
            auto it = cache.find(res_key);
            if (it != cache.end()) return it->second;
            const char* json = get_res(res_key);
            alx::json_object obj;
            if (json) {
                bool ok = false;
                alx::json_value v = alx::json_doc::from_value(json, &ok);
                if (ok && v.is<alx::json_object>()) obj = v.to<alx::json_object>();
            }
            auto [ins, _] = cache.emplace(res_key, std::move(obj));
            return ins->second;
        }
    }

    alx::json_object extend::docs(const std::string& _re) const {
        return filter(docs(), _re);
    }

    alx::json_object extend::filter(const alx::json_object& _src, const std::string& _re) {
        alx::json_object out;
        alx::regex_ex re(_re);
        if (!re.is_valid()) return out;
        auto keep = [&](const char* _key) {
            if (!_src.contain(_key)) return;
            alx::json_object kept;
            auto& src = _src.value(_key).to<alx::json_object>();
            for (auto it = src.begin(); it != src.end(); ++it)
                if (!re.find(it.key()).empty()) kept[it.key()] = it.value();
            if (!kept.empty()) out[_key] = alx::json_value(std::move(kept));
        };
        keep("defines");
        keep("functions");
        return out;
    }

    // --- utils: is_literal / assert_literal / handle_name / repr / display ---

    namespace utils {

        const char* handle_name(const alx::anyptr& _p) {
            return inspect::name(_p);
        }

        handle_parts handle_decode(const alx::anyptr& _p) {
            static_assert(sizeof(alx::anyptr) == 3 * sizeof(std::uintptr_t), "anyptr layout changed");
            std::uintptr_t words[3] = {0, 0, 0};
            std::memcpy(words, &_p, sizeof(words));
            return {words[0], words[1], words[2]};
        }

        // handle_hex — the three pointers with names, fixed width; null when the object is empty
        std::string handle_hex(const alx::anyptr& _p) {
            if (_p.null()) return "null";
            const handle_parts p = handle_decode(_p);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "obj=0x%016llx ref=0x%016llx reg=0x%016llx",
                          (unsigned long long) p.object, (unsigned long long) p.ref,
                          (unsigned long long) p.reg);
            return buf;
        }

        // fmt_double — the engine's string(double) rule, mirrored (ascript_utils is not packaged):
        // %.6f, trailing zeros dropped but one kept; |v| < 5e-7 goes through %g so it stays nonzero
        static std::string fmt_double(double _v) {
            char buf[512];
            double mag = _v < 0 ? -_v : _v;
            if (mag != 0.0 && mag < 0.0000005) {
                snprintf(buf, sizeof(buf), "%g", _v);
                return std::string(buf);
            }
            snprintf(buf, sizeof(buf), "%.6f", _v);
            std::string s(buf);
            size_t dot = s.find('.');
            if (dot == std::string::npos) return s;
            size_t end = s.size();
            while (end > dot + 2 && s[end - 1] == '0') end--;
            return s.substr(0, end);
        }

        // repr_of — the recursive body of repr (strings quoted at every level)
        static std::string repr_of(const alx::variant& _v) {
            if (_v.null()) return "null";
            if (_v.is<bool>()) return _v.to<bool>() ? "true" : "false";
            if (_v.is<alx::int_64>()) return std::to_string(_v.to<alx::int_64>());
            if (_v.is<double>()) return fmt_double(_v.to<double>());
            if (_v.is<std::string>()) return "\"" + _v.to<std::string>() + "\"";
            if (_v.is<alx::bytes>()) return "bytes(" + std::to_string(_v.to<alx::bytes>().size()) + ")";
            if (_v.is<alx::varvec>()) {
                const auto& v = _v.to<alx::varvec>();
                std::string s = "[";
                for (size_t i = 0; i < v.size(); i++) {
                    if (i) s += ", ";
                    s += repr_of(v[i]);
                }
                return s + "]";
            }
            if (_v.is<alx::varlst>()) {
                std::string s = "[";
                bool first = true;
                for (const auto& e : _v.to<alx::varlst>()) {
                    if (!first) s += ", ";
                    first = false;
                    s += repr_of(e);
                }
                return s + "]";
            }
            if (_v.is<alx::varmap>()) {
                const auto& m = _v.to<alx::varmap>();
                std::string s = "{";
                bool first = true;
                for (auto it = m.begin(); it != m.end(); ++it) {
                    if (!first) s += ", ";
                    first = false;
                    s += it.key() + ": " + repr_of(it.value());
                }
                return s + "}";
            }
            if (_v.is<alx::anyptr>()) {
                const alx::anyptr& p = _v.to<alx::anyptr>();
                if (p.null()) return "null";
                const char* n = handle_name(p);
                return n ? "<" + std::string(n) + ">" : "<handle>";
            }
            return "?";
        }

        std::string repr(const alx::variant& _v) {
            return repr_of(_v);
        }

        std::string display(const alx::variant& _v) {
            if (_v.is<std::string>()) return _v.to<std::string>();
            return repr_of(_v);
        }

        bool is_literal(const alx::variant& _v) {
            if (_v.is<alx::anyptr>()) return false;
            if (_v.is<alx::varmap>()) {
                const auto& m = _v.to<alx::varmap>();
                for (auto it = m.begin(); it != m.end(); ++it)
                    if (!is_literal(it.value())) return false;
                return true;
            }
            if (_v.is<alx::varvec>()) {
                for (const auto& e : _v.to<alx::varvec>())
                    if (!is_literal(e)) return false;
                return true;
            }
            if (_v.is<alx::varlst>()) {
                for (const auto& e : _v.to<alx::varlst>())
                    if (!is_literal(e)) return false;
                return true;
            }
            return true;
        }

    } // namespace utils

}
