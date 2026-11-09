#include "core.h"
#include "extend.h"
#include "res.h"

#include "aregex_ex.h"
#include "impl/bytes.h"
#include "impl/evt.h"
#include "impl/fs.h"
#include "impl/global.h"
#include "impl/hash.h"
#include "impl/io.h"
#include "impl/kv.h"
#include "impl/lst.h"
#include "impl/map.h"
#include "impl/math.h"
#include "impl/regx.h"
#include "impl/str.h"
#include "impl/sys.h"
#include "impl/thd.h"
#include "impl/vec.h"

#include <stdexcept>
#include <string>

namespace core {

    // all extend modules — explicit registration
    static const extend* s_modules[] = {
        &global::instance(),
        &io::instance(),
        &sys::instance(),
        &math::instance(),
        &vec::instance(),
        &lst::instance(),
        &map::instance(),
        &fs::instance(),
        &str::instance(),
        &regx::instance(),
        &bytes::instance(),
        &hash::instance(),
        &kv::instance(),
        &thd::instance(),
        &evt::instance(),
    };

    alx::script::engine_config varmap_to_config(const alx::varmap& _m) {
        alx::script::engine_config cfg;
        if (_m.contain("overflow_check")) cfg.overflow_check = _m.value("overflow_check").to<bool>();
        if (_m.contain("max_stack")) cfg.max_stack = _m.value("max_stack").to<alx::uint_64>();
        if (_m.contain("max_vecfill")) cfg.max_vecfill = _m.value("max_vecfill").to<alx::uint_64>();
        if (_m.contain("parse_depth")) cfg.parse_depth = _m.value("parse_depth").to<alx::uint_64>();
        if (_m.contain("debug_enable")) cfg.debug_enable = _m.value("debug_enable").to<bool>();
        return cfg;
    }

    engine* create(const alx::script::engine_config& _cfg) {
        auto* e = alx::script::engine::create(_cfg);
        for (const auto* mod : s_modules)
            mod->install(e);
        return e;
    }

    void destroy(engine* _e) {
        delete _e;
    }

    const char* version() {
        return ENGINE_VERSION;
    }

    static void merge_doc(alx::json_object& _defines, alx::json_object& _functions,
                          const alx::json_object& _src) {
        if (_src.contain("defines")) {
            auto& sd = _src.value("defines").to<alx::json_object>();
            for (auto it = sd.begin(); it != sd.end(); ++it) _defines[it.key()] = it.value();
        }
        auto& sf = _src.value("functions").to<alx::json_object>();
        for (auto it = sf.begin(); it != sf.end(); ++it) _functions[it.key()] = it.value();
    }

    static const alx::json_object& langDocs() {
        static const alx::json_object s_docs = [] {
            const char* json = get_res("keywords.json");
            if (!json) return alx::json_object();
            bool ok = false;
            alx::json_value v = alx::json_doc::from_value(json, &ok);
            if (!ok || !v.is<alx::json_object>()) return alx::json_object();
            return v.to<alx::json_object>();
        }();
        return s_docs;
    }

    static alx::json_object langDocs(const std::string& _re) {
        alx::json_object out;
        alx::regex_ex re(_re);
        if (!re.is_valid()) return out;
        for (const char* key : {"keywords", "literals", "builtins"}) {
            if (!langDocs().contain(key)) continue;
            const auto& src = langDocs().value(key).to<alx::json_object>();
            alx::json_object kept;
            for (auto it = src.begin(); it != src.end(); ++it)
                if (!re.find(it.key()).empty()) kept[it.key()] = it.value();
            if (!kept.empty()) out[key] = alx::json_value(std::move(kept));
        }
        return out;
    }

    const alx::json_object& document() {
        static const alx::json_object s_docs = [] {
            alx::json_object o;
            alx::json_object defines;
            alx::json_object functions;
            for (const auto* mod : s_modules)
                merge_doc(defines, functions, mod->docs());
            if (!defines.empty()) o["defines"] = alx::json_value(std::move(defines));
            o["functions"] = alx::json_value(std::move(functions));
            for (const char* key : {"keywords", "literals", "builtins"})
                if (langDocs().contain(key)) o[key] = langDocs().value(key);
            return o;
        }();
        return s_docs;
    }

    alx::json_object document(const std::string& _re) {
        alx::json_object o;
        alx::json_object defines;
        alx::json_object functions;
        for (const auto* mod : s_modules)
            merge_doc(defines, functions, mod->docs(_re));
        if (!defines.empty()) o["defines"] = alx::json_value(std::move(defines));
        o["functions"] = alx::json_value(std::move(functions));
        const auto lang = langDocs(_re);
        for (const char* key : {"keywords", "literals", "builtins"})
            if (lang.contain(key)) o[key] = lang.value(key);
        return o;
    }

    const alx::json_array& langref() {
        static const alx::json_array s_blocks = [] {
            const char* json = get_res("lang.json");
            if (!json) throw std::runtime_error("core: lang.json resource is missing");
            bool ok = false;
            const alx::json_value v = alx::json_doc::from_value(json, &ok);
            if (!ok || !v.is<alx::json_object>())
                throw std::runtime_error("core: lang.json is not a JSON object");
            const auto& o = v.to<alx::json_object>();
            if (!o.contain("blocks") || !o.value("blocks").is<alx::json_array>())
                throw std::runtime_error("core: lang.json has no blocks array");
            return o.value("blocks").to<alx::json_array>();
        }();
        return s_blocks;
    }

} // namespace core
