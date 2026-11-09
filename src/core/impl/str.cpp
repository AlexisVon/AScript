#include "str.h"

#include "astring.h"

#include <algorithm>
#include <cctype>

// $str_xxx extensions — string operations
namespace core {
    namespace str_impl {

        using utils::as_str;
        using utils::as_int;

        static void ext_find(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_find str");
            const auto& tag = as_str(fw[1], "str_find tag");
            alx::uint_64 from = fw.size() > 2 ? static_cast<alx::uint_64>(as_int(fw[2], "str_find from")) : 0;
            size_t pos = alx::strutil::find(str, tag, from);
            fw.freturn(alx::variant(static_cast<alx::int_64>(pos == alx::uint_64_npos ? -1 : pos)));
        }

        static void ext_rfind(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_rfind str");
            const auto& tag = as_str(fw[1], "str_rfind tag");
            size_t pos = alx::strutil::rfind(str, tag);
            fw.freturn(alx::variant(static_cast<alx::int_64>(pos == alx::uint_64_npos ? -1 : pos)));
        }

        static void ext_left(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_left str");
            auto n = static_cast<size_t>(as_int(fw[1], "str_left n"));
            if (n >= str.size())
                fw.freturn(alx::variant(str));
            else
                fw.freturn(alx::variant(str.substr(0, n)));
        }

        static void ext_right(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_right str");
            auto n = static_cast<size_t>(as_int(fw[1], "str_right n"));
            if (n >= str.size())
                fw.freturn(alx::variant(str));
            else
                fw.freturn(alx::variant(str.substr(str.size() - n)));
        }

        static void ext_mid(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_mid str");
            auto pos = static_cast<size_t>(as_int(fw[1], "str_mid pos"));
            if (pos >= str.size()) {
                fw.freturn(alx::variant(std::string()));
                return;
            }
            if (fw.size() > 2) {
                auto len = static_cast<size_t>(as_int(fw[2], "str_mid len"));
                fw.freturn(alx::variant(str.substr(pos, len)));
            } else {
                fw.freturn(alx::variant(str.substr(pos)));
            }
        }

        static void ext_split(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_split str");
            const auto& delim = as_str(fw[1], "str_split delim");
            auto lst = alx::strutil::split(str, delim);
            alx::varvec v;
            for (auto& s : lst) v.push_back(alx::variant(std::move(s)));
            fw.freturn(alx::variant(std::move(v)));
        }

        static void ext_join(alx::script::fwrap& fw) {
            std::list<std::string> parts;
            if (fw[0].is<alx::varvec>()) {
                for (auto& v : fw[0].to<alx::varvec>()) parts.push_back(v.to<std::string>());
            } else if (fw[0].is<alx::varlst>()) {
                for (auto& v : fw[0].to<alx::varlst>()) parts.push_back(v.to<std::string>());
            } else {
                throw alx::script::script_exception{alx::script::error_type::TypeError,
                                                    "str_join parts: expected vec or lst"};
            }
            const auto& delim = as_str(fw[1], "str_join delim");
            fw.freturn(alx::variant(alx::strutil::join(parts, delim)));
        }

        static void ext_replace(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_replace str");
            const auto& old_s = as_str(fw[1], "str_replace old");
            const auto& new_s = as_str(fw[2], "str_replace new");
            if (old_s.empty()) {
                fw.freturn(alx::variant(str));
                return;
            }
            std::string result;
            size_t pos = 0, last = 0;
            while ((pos = str.find(old_s, last)) != std::string::npos) {
                result.append(str, last, pos - last);
                result.append(new_s);
                last = pos + old_s.size();
            }
            result.append(str, last, std::string::npos);
            fw.freturn(alx::variant(std::move(result)));
        }

        static void ext_upper(alx::script::fwrap& fw) {
            std::string s = as_str(fw[0], "str_upper str");
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
            fw.freturn(alx::variant(std::move(s)));
        }

        static void ext_lower(alx::script::fwrap& fw) {
            std::string s = as_str(fw[0], "str_lower str");
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
            fw.freturn(alx::variant(std::move(s)));
        }

        static void ext_trim(alx::script::fwrap& fw) {
            std::string s = as_str(fw[0], "str_trim str");
            std::string chars = fw.size() > 1 ? as_str(fw[1], "str_trim chars") : " \t\n\r";
            auto begin = s.find_first_not_of(chars);
            if (begin == std::string::npos) {
                fw.freturn(alx::variant(std::string()));
                return;
            }
            auto end = s.find_last_not_of(chars);
            fw.freturn(alx::variant(s.substr(begin, end - begin + 1)));
        }

        static void ext_starts(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_starts str");
            const auto& prefix = as_str(fw[1], "str_starts prefix");
            fw.freturn(alx::variant(str.size() >= prefix.size() &&
                                    str.compare(0, prefix.size(), prefix) == 0));
        }

        static void ext_ends(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_ends str");
            const auto& suffix = as_str(fw[1], "str_ends suffix");
            fw.freturn(alx::variant(str.size() >= suffix.size() &&
                                    str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0));
        }

        static void ext_contains(alx::script::fwrap& fw) {
            const auto& str = as_str(fw[0], "str_contains str");
            const auto& sub = as_str(fw[1], "str_contains sub");
            fw.freturn(alx::variant(str.find(sub) != std::string::npos));
        }

    } // namespace str_impl

    void str::install(alx::script::engine* _e) const {
        _e->set_extend("str_find", str_impl::ext_find);
        _e->set_extend("str_rfind", str_impl::ext_rfind);
        _e->set_extend("str_left", str_impl::ext_left);
        _e->set_extend("str_right", str_impl::ext_right);
        _e->set_extend("str_mid", str_impl::ext_mid);
        _e->set_extend("str_split", str_impl::ext_split);
        _e->set_extend("str_join", str_impl::ext_join);
        _e->set_extend("str_replace", str_impl::ext_replace);
        _e->set_extend("str_upper", str_impl::ext_upper);
        _e->set_extend("str_lower", str_impl::ext_lower);
        _e->set_extend("str_trim", str_impl::ext_trim);
        _e->set_extend("str_starts", str_impl::ext_starts);
        _e->set_extend("str_ends", str_impl::ext_ends);
        _e->set_extend("str_contains", str_impl::ext_contains);
    }

    const char* str::doc_path() const {
        return "str.json";
    }

} // namespace core
