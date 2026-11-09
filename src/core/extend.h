#ifndef ASEXTEND_H
#define ASEXTEND_H

#include "global.h"

#include "ascript.h"
#include "ajson.h"
#include "abytes.h"
#include "aanyptr.h"

#include <cstdint>

namespace core {
    class extend {
    public:
        virtual ~extend() = default;
        virtual void install(alx::script::engine* _e) const = 0;
        // docs — load module doc from qrc JSON (lazy, parsed once per module)
        const alx::json_object& docs() const;
        // docs(re) — filtered by regex on extend/define names (empty = all)
        alx::json_object docs(const std::string& _re) const;

    protected:
        // doc_path — resource key for this module's doc JSON (e.g. "io.json")
        virtual const char* doc_path() const = 0;

    private:
        static alx::json_object filter(const alx::json_object& _src, const std::string& _re);
    };

    // type-check helpers for extend implementations
    // _desc = function name + param name, e.g. "str_find tag"
    namespace utils {
        inline std::string& as_str(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<std::string>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected string"};
            return _v.as<std::string>();
        }

        inline alx::int_64 as_int(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::int_64>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected int"};
            return _v.as<alx::int_64>();
        }

        inline double as_float(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<double>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected float"};
            return _v.as<double>();
        }

        // as_number — accepts int or float, returns double
        inline double as_number(alx::variant& _v, const std::string& _desc) {
            if (_v.is<double>()) return _v.as<double>();
            if (_v.is<alx::int_64>()) return static_cast<double>(_v.as<alx::int_64>());
            throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected number"};
        }

        inline bool as_bool(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<bool>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected bool"};
            return _v.as<bool>();
        }

        inline alx::bytes& as_bytes(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::bytes>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected bytes"};
            return _v.as<alx::bytes>();
        }

        inline alx::varvec& as_vec(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::varvec>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected vec"};
            return _v.as<alx::varvec>();
        }

        inline alx::varlst& as_lst(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::varlst>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected lst"};
            return _v.as<alx::varlst>();
        }

        inline alx::varmap& as_map(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::varmap>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected map"};
            return _v.as<alx::varmap>();
        }

        // as_handle — a non-anyptr must raise here: the cast dereferences the register tag
        template <typename T>
        inline T* as_handle(alx::variant& _v, const std::string& _desc) {
            if (!_v.is<alx::anyptr>())
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": expected handle"};
            T* p = alx::anyptr_ex<T>::as(_v.as<alx::anyptr>());
            if (!p)
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": handle type mismatch"};
            return p;
        }

        // is_literal — recursively reject anyptr in variant (safe for cross-engine transfer)
        bool is_literal(const alx::variant& _v);
        inline void assert_literal(const alx::variant& _v, const std::string& _desc) {
            if (!is_literal(_v))
                throw alx::script::script_exception{alx::script::error_type::TypeError, _desc + ": anyptr not allowed"};
        }

        // handle_name — printable name of a host handle ("ostream"/"istream"/"thread"); null when unknown
        AXCCORE_API const char* handle_name(const alx::anyptr& _p);

        // handle_parts — the three pointers in an anyptr: object, refcount block, type registration
        struct handle_parts {
            std::uintptr_t object = 0;
            std::uintptr_t ref = 0;
            std::uintptr_t reg = 0;
        };
        // handle_decode — read the object representation; layout-dependent, no dereference
        handle_parts AXCCORE_API handle_decode(const alx::anyptr& _p);
        // handle_hex — "obj=0x.. ref=0x.. reg=0x.." ("null" when empty); debugger only
        std::string AXCCORE_API handle_hex(const alx::anyptr& _p);

        // repr — value to printable string: containers recurse, strings quoted, handles as <name>
        std::string AXCCORE_API repr(const alx::variant& _v);
        // display — repr with a top-level string left unquoted (the $io_print form)
        std::string AXCCORE_API display(const alx::variant& _v);
    } // namespace utils
}

#endif // !ASEXTEND_H
