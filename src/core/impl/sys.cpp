#include "sys.h"

#include "adatetime.h"
#include "aplatform.h"

// $sys_xxx extensions — external process + time
namespace core {
    namespace sys_impl {

        static void ext_exec(alx::script::fwrap& fw) {
            std::string cmd = fw[0].to<std::string>();
            alx::uint_32 timeout = fw.size() > 1 ? static_cast<alx::uint_32>(fw[1].to<alx::int_64>(5000)) : 5000;
            auto r = alx::exec_sync(cmd, timeout);
            alx::varmap result;
            result["output"] = alx::variant(r.output);
            result["excode"] = alx::variant(static_cast<alx::int_64>(r.excode));
            result["success"] = alx::variant(r.success);
            fw.freturn(alx::variant(std::move(result)));
        }

        static void ext_sleep(alx::script::fwrap& fw) {
            if (fw.size() < 1 || !fw[0].is<alx::int_64>())
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "sys_sleep: expected milliseconds"};
            alx::high_precision_sleep(static_cast<alx::uint_64>(fw[0].to<alx::int_64>()));
        }

        static void ext_datetime(alx::script::fwrap& fw) {
            alx::datetime dt = alx::datetime::current();
            alx::varmap m;
            m["year"] = alx::variant(alx::int_64(dt.year()));
            m["month"] = alx::variant(alx::int_64(dt.month()));
            m["day"] = alx::variant(alx::int_64(dt.day()));
            m["h"] = alx::variant(alx::int_64(dt.hour()));
            m["m"] = alx::variant(alx::int_64(dt.minute()));
            m["s"] = alx::variant(alx::int_64(dt.second()));
            m["ms"] = alx::variant(alx::int_64(dt.millisecond()));
            m["us"] = alx::variant(alx::int_64(dt.microsecond()));
            fw.freturn(alx::variant(m));
        }

    } // namespace sys_impl

    void sys::install(alx::script::engine* _e) const {
        _e->set_extend("sys_exec", sys_impl::ext_exec);
        _e->set_extend("sys_datetime", sys_impl::ext_datetime);
        _e->set_extend("sys_sleep", sys_impl::ext_sleep);
    }

    const char* sys::doc_path() const {
        return "sys.json";
    }

} // namespace core
