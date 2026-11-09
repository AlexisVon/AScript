#include "hash.h"

#include "abytes.h"
#include "afile.h"
#include "astream.h"
#include "astream_ex.h"
#include "averify.h"

// $hash_xxx extensions — hashing and checksum
namespace core {
    namespace hash_impl {

        using utils::as_str;
        using utils::as_bytes;

        // hash_data — dispatch bytes or string to verify::exec
        static std::string hash_data(alx::verify::COMMON_TYPE type, alx::script::fwrap& fw) {
            if (fw[0].is<alx::bytes>()) {
                auto& b = as_bytes(fw[0], "hash data");
                return alx::verify::exec(type, reinterpret_cast<const alx::uint_8*>(b.data()), b.size());
            }
            if (fw[0].is<std::string>())
                return alx::verify::exec(type, as_str(fw[0], "hash data"));
            throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                "hash data: expected bytes or string"};
        }

        static void ext_crc32c(alx::script::fwrap& fw) {
            fw.freturn(alx::variant(hash_data(alx::verify::CRC_32C, fw)));
        }

        static void ext_sha1(alx::script::fwrap& fw) {
            fw.freturn(alx::variant(hash_data(alx::verify::SHA_1, fw)));
        }

        static void ext_sha256(alx::script::fwrap& fw) {
            fw.freturn(alx::variant(hash_data(alx::verify::SHA_256, fw)));
        }

        static void ext_file(alx::script::fwrap& fw) {
            auto path = as_str(fw[0], "hash_file path");
            std::string type = fw.size() > 1 ? as_str(fw[1], "hash_file type") : "sha256";
            // map user-friendly names to factory-registered names
            if (type == "sha256") type = "sha_256";
            else if (type == "sha1") type = "sha_1";
            else if (type == "crc32c") type = "crc_32_c";
            alx::verify* v = alx::verify::create(type);
            if (!v)
                throw alx::script::script_exception{alx::script::error_type::RuntimeError,
                                                    "hash_file: unknown hash type (use crc32c/sha1/sha256)"};
            alx::file_info fi(path);
            if (!fi.is_exist()) {
                delete v;
                throw alx::script::script_exception{alx::script::error_type::RuntimeError,
                                                    "hash_file: file not found"};
            }
            alx::istream_file is(fi);
            const alx::uint_64 chunk = 65536;
            for (alx::uint_64 off = 0; off < is.total(); off += chunk) {
                alx::bytes_view bv = is.get(off, std::min(chunk, is.total() - off));
                v->update(bv.data(), bv.size());
            }
            fw.freturn(alx::variant(v->hexdigest()));
            delete v;
        }

    } // namespace hash_impl

    void hash::install(alx::script::engine* _e) const {
        _e->set_extend("hash_crc32c", hash_impl::ext_crc32c);
        _e->set_extend("hash_sha1", hash_impl::ext_sha1);
        _e->set_extend("hash_sha256", hash_impl::ext_sha256);
        _e->set_extend("hash_file", hash_impl::ext_file);
    }

    const char* hash::doc_path() const {
        return "hash.json";
    }

} // namespace core
