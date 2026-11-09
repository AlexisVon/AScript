#include "evt.h"

#include <chrono>
#include <limits>
#include <memory>
#include <string>

// $evt_xxx extensions — queue a call into the script's yield() service point
namespace core {
    namespace evt_impl {

        static void ext_after(alx::script::fwrap& fw) {
            if (fw.size() < 2)
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "evt_after: expected milliseconds and a def name"};
            const alx::int_64 ms = utils::as_int(fw[0], "evt_after ms");
            if (ms < 0 || ms > std::numeric_limits<alx::uint_32>::max())
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "evt_after: delay out of range"};
            const std::string name = utils::as_str(fw[1], "evt_after name");

            alx::varvec args;
            args.reserve(fw.size() - 2);
            for (size_t i = 2; i < fw.size(); ++i) args.push_back(fw[i]);

            std::unique_ptr<alx::script::fwrap::callback> cb(fw.currying(name));
            auto fut = cb->call(std::move(args), 0, 0, static_cast<alx::uint_32>(ms));
            // the script cannot reach a yield point while this native runs, so an accepted
            // delivery is never ready; a ready future is the refusal (yield full)
            const bool accepted = fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready;
            if (!accepted) fut.get();
            fw.freturn(alx::variant(accepted));
        }

    } // namespace evt_impl

    void evt::install(alx::script::engine* _e) const {
        _e->set_extend("evt_after", evt_impl::ext_after);
    }

    const char* evt::doc_path() const {
        return "evt.json";
    }

} // namespace core
