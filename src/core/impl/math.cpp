#include "math.h"

#include <cmath>

// $math_xxx — constants, basic arithmetic, power/log, trig, rounding
namespace core {
    namespace math_impl {

        using utils::as_number;

        // arg_count — arity check, raise ArgError on mismatch
        static void arg_count(alx::script::fwrap& fw, size_t _n, const char* _name) {
            if (fw.size() != _n)
                throw alx::script::script_exception{
                    alx::script::error_type::ArgError,
                    std::string("math_") + _name + ": expected " + std::to_string(_n) +
                        " argument(s), got " + std::to_string(fw.size())};
        }

#define MATH_FN1(_name, _expr)                                                  \
    static void ext_##_name(alx::script::fwrap& fw) {                           \
        arg_count(fw, 1, #_name);                                               \
        fw.freturn(alx::variant(_expr(as_number(fw[0], "math_" #_name " x")))); \
    }
#define MATH_FN2(_name, _expr)                                                  \
    static void ext_##_name(alx::script::fwrap& fw) {                           \
        arg_count(fw, 2, #_name);                                               \
        fw.freturn(alx::variant(_expr(as_number(fw[0], "math_" #_name " a"),    \
                                      as_number(fw[1], "math_" #_name " b")))); \
    }

        MATH_FN1(abs, std::fabs)
        MATH_FN1(sqrt, std::sqrt)
        MATH_FN1(cbrt, std::cbrt)
        MATH_FN1(exp, std::exp)
        MATH_FN1(ln, std::log)
        MATH_FN1(log2, std::log2)
        MATH_FN1(log10, std::log10)
        MATH_FN1(sin, std::sin)
        MATH_FN1(cos, std::cos)
        MATH_FN1(tan, std::tan)
        MATH_FN1(asin, std::asin)
        MATH_FN1(acos, std::acos)
        MATH_FN1(atan, std::atan)
        MATH_FN1(floor, std::floor)
        MATH_FN1(ceil, std::ceil)
        MATH_FN1(round, std::round)
        MATH_FN1(trunc, std::trunc)
        MATH_FN1(sign, [](double _x) { return _x > 0 ? 1.0 : (_x < 0 ? -1.0 : 0.0); })
        MATH_FN1(rad, [](double _d) { return _d * 3.141592653589793 / 180.0; })
        MATH_FN1(deg, [](double _r) { return _r * 180.0 / 3.141592653589793; })

        MATH_FN2(min, std::fmin)
        MATH_FN2(max, std::fmax)
        MATH_FN2(pow, std::pow)
        MATH_FN2(atan2, std::atan2)
        MATH_FN2(hypot, std::hypot)
        MATH_FN2(fmod, std::fmod)

        static void ext_clamp(alx::script::fwrap& fw) {
            arg_count(fw, 3, "clamp");
            double v = as_number(fw[0], "math_clamp v");
            double lo = as_number(fw[1], "math_clamp lo");
            double hi = as_number(fw[2], "math_clamp hi");
            fw.freturn(alx::variant(std::fmin(std::fmax(v, lo), hi)));
        }

#undef MATH_FN1
#undef MATH_FN2

    } // namespace math_impl

    void math::install(alx::script::engine* _e) const {
        _e->set_define("math_PI", alx::variant(3.141592653589793));
        _e->set_define("math_E", alx::variant(2.718281828459045));
        _e->set_extend("math_abs", math_impl::ext_abs);
        _e->set_extend("math_sqrt", math_impl::ext_sqrt);
        _e->set_extend("math_cbrt", math_impl::ext_cbrt);
        _e->set_extend("math_exp", math_impl::ext_exp);
        _e->set_extend("math_ln", math_impl::ext_ln);
        _e->set_extend("math_log2", math_impl::ext_log2);
        _e->set_extend("math_log10", math_impl::ext_log10);
        _e->set_extend("math_sin", math_impl::ext_sin);
        _e->set_extend("math_cos", math_impl::ext_cos);
        _e->set_extend("math_tan", math_impl::ext_tan);
        _e->set_extend("math_asin", math_impl::ext_asin);
        _e->set_extend("math_acos", math_impl::ext_acos);
        _e->set_extend("math_atan", math_impl::ext_atan);
        _e->set_extend("math_floor", math_impl::ext_floor);
        _e->set_extend("math_ceil", math_impl::ext_ceil);
        _e->set_extend("math_round", math_impl::ext_round);
        _e->set_extend("math_trunc", math_impl::ext_trunc);
        _e->set_extend("math_sign", math_impl::ext_sign);
        _e->set_extend("math_rad", math_impl::ext_rad);
        _e->set_extend("math_deg", math_impl::ext_deg);
        _e->set_extend("math_min", math_impl::ext_min);
        _e->set_extend("math_max", math_impl::ext_max);
        _e->set_extend("math_pow", math_impl::ext_pow);
        _e->set_extend("math_atan2", math_impl::ext_atan2);
        _e->set_extend("math_hypot", math_impl::ext_hypot);
        _e->set_extend("math_fmod", math_impl::ext_fmod);
        _e->set_extend("math_clamp", math_impl::ext_clamp);
    }

    const char* math::doc_path() const {
        return "math.json";
    }

} // namespace core
