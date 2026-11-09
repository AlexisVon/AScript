#include "io.h"

#include "abytes.h"
#include "afile.h"
#include "ajson.h"
#include "astream.h"
#include "astring.h"

#include <iostream>

// $io_xxx extensions — terminal + file + JSON data I/O
namespace core {
    namespace io_impl {

        static void ext_print(alx::script::fwrap& fw) {
            std::vector<std::string> args;
            for (size_t i = 0; i < fw.size(); i++) {
                args.push_back(core::utils::display(fw[i]));
            }
            std::string output;
            if (args.size() == 1) {
                output = args[0];
            } else if (args.size() > 1) {
                std::vector<std::string> fmt_args(args.begin() + 1, args.end());
                output = alx::strutil::format(args[0], fmt_args);
            }
            if (!output.empty()) {
                if (alx::script::pipe_out po = fw.config().pipe_out_ptr) po(&output, fw.config().pipe_out_ud);
                else std::cout << output << std::flush;
            }
        }

        static void ext_input(alx::script::fwrap& fw) {
            std::string line;
            if (alx::script::pipe_in pi = fw.config().pipe_in_ptr) line = pi(fw.config().pipe_in_ud);
            else if (!std::getline(std::cin, line)) line = "";
            fw.freturn(alx::variant(std::move(line)));
        }

        static void ext_fread(alx::script::fwrap& fw) {
            std::string path = fw[0].to<std::string>();
            alx::file_info fi(path);
            fw.freturn(alx::variant(fi.is_exist() ? alx::file::read_all(fi) : alx::bytes()));
        }

        static void ext_fwrite(alx::script::fwrap& fw) {
            std::string path = fw[0].to<std::string>();
            bool ok = fw[1].is<alx::bytes>() ? alx::file::write_all(path, fw[1].to<alx::bytes>(), false)
                                             : alx::file::write_all(path, fw[1].to<std::string>(), false);
            fw.freturn(alx::variant(ok));
        }

        // read — $io_read(h) -> whole; $io_read(h, ofst) -> [ofst, end)
        static void ext_read(alx::script::fwrap& fw) {
            alx::istream* s = utils::as_handle<alx::istream>(fw[0], "io_read: handle from $fs_open");
            alx::int_64 ofst = fw.size() > 1 ? fw[1].to<alx::int_64>() : 0;
            if (ofst < 0) ofst = 0; // to<uint_64> unsupported (cross-sign); clamp here
            alx::uint_64 total = s->total();
            if (static_cast<alx::uint_64>(ofst) >= total) {
                fw.freturn(alx::variant(alx::bytes()));
                return;
            }
            alx::bytes_view bv = s->get(static_cast<alx::uint_64>(ofst), total - static_cast<alx::uint_64>(ofst));
            fw.freturn(alx::variant(bv.to_bytes()));
        }

        // write — $io_write(h, data) -> append + flush, bool
        static void ext_write(alx::script::fwrap& fw) {
            alx::ostream* s = utils::as_handle<alx::ostream>(fw[0], "io_write: handle from $fs_open");
            bool ok = false;
            if (fw[1].is<alx::bytes>()) {
                auto& b = fw[1].to<alx::bytes>();
                ok = s->append(reinterpret_cast<const char*>(b.data()), b.size());
            } else {
                std::string data = fw[1].to<std::string>();
                ok = s->append(data.data(), data.size());
            }
            s->flush();
            fw.freturn(alx::variant(ok));
        }

        static void ext_tojs(alx::script::fwrap& fw) {
            if (fw.size() < 1) {
                fw.freturn(alx::variant(std::string("null")));
                return;
            }
            const auto& v = fw[0];
            bool compact = fw.size() > 1 && fw[1].is<bool>() && fw[1].to<bool>();
            fw.freturn(alx::variant(alx::json_doc::to_string(alx::json_value::from_variant(v), compact)));
        }

        static void ext_fmjs(alx::script::fwrap& fw) {
            if (fw.size() < 1 || !fw[0].is<std::string>()) {
                fw.freturn();
                return;
            }
            bool ok = false;
            alx::json_value jv = alx::json_doc::from_value(fw[0].to<std::string>(), &ok);
            if (!ok) {
                fw.freturn();
                return;
            }
            fw.freturn(jv.to_variant());
        }

    } // namespace io_impl

    void io::install(alx::script::engine* _e) const {
        _e->set_extend("io_print", io_impl::ext_print);
        _e->set_extend("io_input", io_impl::ext_input);
        _e->set_extend("io_fread", io_impl::ext_fread);
        _e->set_extend("io_fwrite", io_impl::ext_fwrite);
        _e->set_extend("io_read", io_impl::ext_read);
        _e->set_extend("io_write", io_impl::ext_write);
        _e->set_extend("io_tojs", io_impl::ext_tojs);
        _e->set_extend("io_fmjs", io_impl::ext_fmjs);
    }

    const char* io::doc_path() const {
        return "io.json";
    }

} // namespace core
