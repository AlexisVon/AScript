#include "fs.h"

#include "../inspect.h"

#include "afile.h"
#include "astream.h"
#include "astream_ex.h"

// $fs_xxx — file/dir operations (alexis afile): create/rmfile/mkdir/rmdir
// return bool (false on failure); info/parent -> map, child -> vec<map>.
namespace core {
    namespace fs_impl {

        // str_arg — string path arg at fw[_idx]; ArgError otherwise
        static std::string str_arg(alx::script::fwrap& fw, size_t _idx, const char* _fn) {
            if (fw.size() <= _idx || !fw[_idx].is<std::string>())
                throw alx::script::script_exception{
                    alx::script::error_type::ArgError,
                    std::string(_fn) + ": expected a string path at argument " + std::to_string(_idx + 1)};
            return fw[_idx].to<std::string>();
        }

        // info_map — file_info -> script map (all scalar fields)
        static alx::varmap info_map(const alx::file_info& _i) {
            alx::varmap m;
            m["name"] = alx::variant(_i.name());
            m["path"] = alx::variant(_i.path());
            m["suffix"] = alx::variant(_i.suffix());
            m["size"] = alx::variant(static_cast<alx::int_64>(_i.size()));
            m["time_ct"] = alx::variant(static_cast<alx::int_64>(_i.time_ct()));
            m["time_la"] = alx::variant(static_cast<alx::int_64>(_i.time_la()));
            m["time_lw"] = alx::variant(static_cast<alx::int_64>(_i.time_lw()));
            m["dir"] = alx::variant(_i.is_dir());
            m["exist"] = alx::variant(_i.is_exist());
            m["link"] = alx::variant(_i.is_link());
            return m;
        }

        // open — path+mode -> stream handle (anyptr, shared: closes when the last copy drops)
        static void ext_open(alx::script::fwrap& fw) {
            std::string p = str_arg(fw, 0, "fs_open");
            std::string mode = fw.size() > 1 && fw[1].is<std::string>() ? fw[1].to<std::string>() : "r";
            if (mode == "w" || mode == "a") {
                auto* s = new alx::ostream_file(alx::file_info(p), mode == "a");
                fw.freturn(alx::variant(alx::anyptr_ex<alx::ostream>::make_ref(s)));
            } else if (mode == "r") {
                auto* s = new alx::istream_file(alx::file_info(p));
                fw.freturn(alx::variant(alx::anyptr_ex<alx::istream>::make_ref(s)));
            } else {
                throw alx::script::script_exception{alx::script::error_type::ArgError,
                                                    "fs_open: expected mode r/w/a"};
            }
        }

        static std::string ostream_desc(const alx::anyptr& _p) {
            alx::ostream* s = alx::anyptr_ex<alx::ostream>::as(_p);
            return s ? s->name() : std::string();
        }

        static std::string istream_desc(const alx::anyptr& _p) {
            alx::istream* s = alx::anyptr_ex<alx::istream>::as(_p);
            return s ? s->name() : std::string();
        }

        static void ext_rmfile(alx::script::fwrap& fw) {
            fw.freturn(alx::variant(alx::file_info::rmfile(str_arg(fw, 0, "fs_rmfile"))));
        }

        static void ext_mkdir(alx::script::fwrap& fw) {
            fw.freturn(alx::variant(alx::file_info::mkdir(str_arg(fw, 0, "fs_mkdir"))));
        }

        static void ext_rmdir(alx::script::fwrap& fw) {
            fw.freturn(alx::variant(alx::file_info::rmdir(str_arg(fw, 0, "fs_rmdir"))));
        }

        static void ext_info(alx::script::fwrap& fw) {
            alx::file_info i(str_arg(fw, 0, "fs_info"));
            fw.freturn(alx::variant(info_map(i)));
        }

        static void ext_parent(alx::script::fwrap& fw) {
            alx::file_info i = alx::file_info(str_arg(fw, 0, "fs_parent")).get_parent();
            fw.freturn(alx::variant(info_map(i)));
        }

        static void ext_child(alx::script::fwrap& fw) {
            std::string p = str_arg(fw, 0, "fs_child");
            std::string filter = fw.size() > 1 && fw[1].is<std::string>() ? fw[1].to<std::string>() : "*";
            alx::file_info d(p);
            alx::varvec result;
            if (d.is_dir())
                for (const auto& c : d.get_child(filter)) result.push_back(alx::variant(info_map(c)));
            fw.freturn(alx::variant(std::move(result)));
        }

    } // namespace fs_impl

    void fs::install(alx::script::engine* _e) const {
        inspect::add(alx::anyptr_ex<alx::ostream>::rid, "ostream", fs_impl::ostream_desc, nullptr);
        inspect::add(alx::anyptr_ex<alx::istream>::rid, "istream", fs_impl::istream_desc, nullptr);
        _e->set_extend("fs_open", fs_impl::ext_open);
        _e->set_extend("fs_rmfile", fs_impl::ext_rmfile);
        _e->set_extend("fs_mkdir", fs_impl::ext_mkdir);
        _e->set_extend("fs_rmdir", fs_impl::ext_rmdir);
        _e->set_extend("fs_info", fs_impl::ext_info);
        _e->set_extend("fs_parent", fs_impl::ext_parent);
        _e->set_extend("fs_child", fs_impl::ext_child);
    }

    const char* fs::doc_path() const {
        return "fs.json";
    }

} // namespace core
