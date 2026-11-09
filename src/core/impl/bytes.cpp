#include "bytes.h"

#include "abytes.h"

// $bytes_xxx extensions — binary data operations
namespace core {
    namespace bytes_impl {

        using utils::as_bytes;
        using utils::as_int;

        static void ext_size(alx::script::fwrap& fw) {
            const auto& b = as_bytes(fw[0], "bytes_size b");
            fw.freturn(alx::variant(static_cast<alx::int_64>(b.size())));
        }

        static void ext_find(alx::script::fwrap& fw) {
            const auto& b = as_bytes(fw[0], "bytes_find b");
            alx::bytes needle;
            if (fw[1].is<alx::bytes>())
                needle = fw[1].to<alx::bytes>();
            else if (fw[1].is<std::string>())
                needle = alx::bytes(fw[1].to<std::string>());
            else
                throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                    "bytes_find needle: expected bytes or string"};
            alx::uint_64 from = fw.size() > 2 ? static_cast<alx::uint_64>(as_int(fw[2], "bytes_find from")) : 0;
            alx::uint_64 pos = b.find(needle, from);
            fw.freturn(alx::variant(static_cast<alx::int_64>(pos == alx::max_uint_64 ? -1 : pos)));
        }

        static void ext_mid(alx::script::fwrap& fw) {
            const auto& b = as_bytes(fw[0], "bytes_mid b");
            auto pos = static_cast<alx::uint_64>(as_int(fw[1], "bytes_mid pos"));
            auto len = fw.size() > 2 ? static_cast<alx::uint_64>(as_int(fw[2], "bytes_mid len")) : alx::max_uint_64;
            fw.freturn(alx::variant(b.mid(pos, len)));
        }

        static void ext_left(alx::script::fwrap& fw) {
            const auto& b = as_bytes(fw[0], "bytes_left b");
            auto len = static_cast<alx::uint_64>(as_int(fw[1], "bytes_left n"));
            fw.freturn(alx::variant(b.left(len)));
        }

        static void ext_right(alx::script::fwrap& fw) {
            const auto& b = as_bytes(fw[0], "bytes_right b");
            auto len = static_cast<alx::uint_64>(as_int(fw[1], "bytes_right n"));
            fw.freturn(alx::variant(b.right(len)));
        }

        static void ext_append(alx::script::fwrap& fw) {
            alx::bytes result = as_bytes(fw[0], "bytes_append a");
            if (fw[1].is<alx::bytes>())
                result.append(fw[1].to<alx::bytes>());
            else if (fw[1].is<std::string>())
                result.append(fw[1].to<std::string>());
            else
                throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                    "bytes_append b: expected bytes or string"};
            fw.freturn(alx::variant(std::move(result)));
        }

        static void ext_revers(alx::script::fwrap& fw) {
            alx::bytes result = as_bytes(fw[0], "bytes_revers b");
            result.revers();
            fw.freturn(alx::variant(std::move(result)));
        }

    } // namespace bytes_impl

    void bytes::install(alx::script::engine* _e) const {
        _e->set_extend("bytes_size", bytes_impl::ext_size);
        _e->set_extend("bytes_find", bytes_impl::ext_find);
        _e->set_extend("bytes_mid", bytes_impl::ext_mid);
        _e->set_extend("bytes_left", bytes_impl::ext_left);
        _e->set_extend("bytes_right", bytes_impl::ext_right);
        _e->set_extend("bytes_append", bytes_impl::ext_append);
        _e->set_extend("bytes_revers", bytes_impl::ext_revers);
    }

    const char* bytes::doc_path() const {
        return "bytes.json";
    }

} // namespace core
