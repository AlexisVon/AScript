#include "regx.h"

#include "aregex_ex.h"

// $regx_xxx extensions — regex operations
namespace core {
    namespace regx_impl {

        using utils::as_str;

        // make_re — compile pattern with type + validity checks
        static alx::regex_ex make_re(alx::variant& _v, const std::string& _desc) {
            alx::regex_ex re(as_str(_v, _desc));
            if (!re.is_valid())
                throw alx::script::script_exception{alx::script::error_type::RuntimeError,
                                                    _desc + ": invalid regex pattern"};
            return re;
        }

        static void ext_valid(alx::script::fwrap& fw) {
            alx::regex_ex re(as_str(fw[0], "regx_valid pattern"));
            fw.freturn(alx::variant(re.is_valid()));
        }

        static void ext_match(alx::script::fwrap& fw) {
            auto re = make_re(fw[0], "regx_match pattern");
            fw.freturn(alx::variant(re.is_compliant(as_str(fw[1], "regx_match str"))));
        }

        static void ext_find(alx::script::fwrap& fw) {
            auto re = make_re(fw[0], "regx_find pattern");
            fw.freturn(alx::variant(re.find(as_str(fw[1], "regx_find str"))));
        }

        static void ext_find_all(alx::script::fwrap& fw) {
            auto re = make_re(fw[0], "regx_find_all pattern");
            auto results = re.find_all(as_str(fw[1], "regx_find_all str"));
            alx::varvec v;
            for (auto& s : results) v.push_back(alx::variant(std::move(s)));
            fw.freturn(alx::variant(std::move(v)));
        }

        static void ext_group(alx::script::fwrap& fw) {
            auto re = make_re(fw[0], "regx_group pattern");
            auto groups = re.match_group(as_str(fw[1], "regx_group str"));
            alx::varvec v;
            // skip groups[0] (full match), keep capture groups only
            for (size_t i = 1; i < groups.size(); i++) v.push_back(alx::variant(std::move(groups[i])));
            fw.freturn(alx::variant(std::move(v)));
        }

        static void ext_group_all(alx::script::fwrap& fw) {
            auto re = make_re(fw[0], "regx_group_all pattern");
            auto& str = as_str(fw[1], "regx_group_all str");
            // call once to determine stride (full match + capture groups)
            auto first = re.match_group(str);
            if (first.empty()) {
                fw.freturn(alx::variant(alx::varvec()));
                return;
            }
            size_t stride = first.size(); // 1 + num_groups
            size_t ngroups = stride - 1;
            auto all = re.match_group_all(str);
            alx::varvec outer;
            // first match: reuse 'first', skip [0] (full match)
            {
                alx::varvec group;
                for (size_t j = 1; j < stride; j++)
                    group.push_back(alx::variant(std::move(first[j])));
                outer.push_back(alx::variant(std::move(group)));
            }
            // remaining matches: all[0..] includes first match again, skip first stride elements
            for (size_t i = stride; i + ngroups < all.size(); i += stride) {
                alx::varvec group;
                for (size_t j = 1; j < stride; j++)
                    group.push_back(alx::variant(std::move(all[i + j])));
                outer.push_back(alx::variant(std::move(group)));
            }
            fw.freturn(alx::variant(std::move(outer)));
        }

        static void ext_replace(alx::script::fwrap& fw) {
            auto re = make_re(fw[0], "regx_replace pattern");
            fw.freturn(alx::variant(re.replace(as_str(fw[1], "regx_replace str"), as_str(fw[2], "regx_replace repl"))));
        }

        static void ext_replace_all(alx::script::fwrap& fw) {
            auto re = make_re(fw[0], "regx_replace_all pattern");
            fw.freturn(alx::variant(re.replace_all(as_str(fw[1], "regx_replace_all str"), as_str(fw[2], "regx_replace_all repl"))));
        }

        static void ext_split(alx::script::fwrap& fw) {
            auto re = make_re(fw[0], "regx_split pattern");
            auto& str = as_str(fw[1], "regx_split str");
            std::sregex_token_iterator it(str.begin(), str.end(), *re.get_regex(), -1);
            std::sregex_token_iterator end;
            alx::varvec v;
            for (; it != end; ++it) v.push_back(alx::variant(it->str()));
            fw.freturn(alx::variant(std::move(v)));
        }

    } // namespace regx_impl

    void regx::install(alx::script::engine* _e) const {
        _e->set_extend("regx_valid", regx_impl::ext_valid);
        _e->set_extend("regx_match", regx_impl::ext_match);
        _e->set_extend("regx_find", regx_impl::ext_find);
        _e->set_extend("regx_find_all", regx_impl::ext_find_all);
        _e->set_extend("regx_group", regx_impl::ext_group);
        _e->set_extend("regx_group_all", regx_impl::ext_group_all);
        _e->set_extend("regx_replace", regx_impl::ext_replace);
        _e->set_extend("regx_replace_all", regx_impl::ext_replace_all);
        _e->set_extend("regx_split", regx_impl::ext_split);
    }

    const char* regx::doc_path() const {
        return "regx.json";
    }

} // namespace core
