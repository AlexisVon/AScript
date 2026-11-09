#ifndef ASCORE_H
#define ASCORE_H

#include "global.h"

// core — script engine execution facade.
// Facade: core.h/core.cpp (registration + export); implementations are
// static functions clustered by feature into files under src/core/.
// Feature files are pending user assignment — only the skeleton here.
//
// Scope: create passes config through to engine::create; extend ($xxx
// registration) is core-owned. Hook wiring / exec control / config go
// directly to the returned engine by the caller (cli/gui).
// Pending: extend feature file + "some functions not registrable" control.

namespace core {

    using engine = alx::script::engine;
    // Create engine — config passed through to engine::create
    AXCCORE_API engine* create(const alx::script::engine_config& _cfg = alx::script::engine_config());

    // varmap → engine_config bridge (overflow_check/max_stack/max_vecfill/parse_depth/debug_enable)
    AXCCORE_API alx::script::engine_config varmap_to_config(const alx::varmap& _m);

    // Destroy engine instance
    AXCCORE_API void destroy(engine* _e);

    // Host version string (kEtype, e.g. "axc-1.0.0")
    AXCCORE_API const char* version();

    // ── extend documentation ──────────────────────────────────────────────
    // Collected by core from each feature module's install metadata:
    // {
    //   "defines": {                                // static $ constants, if any
    //     "<name>": {"type": str, "value": scalar, "desc": str}
    //   },
    //   "functions": {                              // $ functions, key = name
    //     "<extend>": [                             // array = overloads
    //       {"desc": str,
    //        "param": [{"key": str, "type": str, "optional"?: bool,
    //                   "default"?: scalar, "desc": str}, ...],
    //        "ret"?: {"type": str, "desc": str}}
    //     ]
    //   },
    //   "keywords" | "literals" | "builtins": [     // engine builtin words (keywords.json)
    //     {"name": str, "desc": str}, ...
    //   ]
    // }
    AXCCORE_API const alx::json_object& document();

    // document(re) — document() filtered by regex on extend/define names.
    // Each module filters its own docs (core::util::filter) before merge.
    AXCCORE_API alx::json_object document(const std::string& _re);

    // langref — the language reference block model, generated from res/core/lang.md
    // by tools/md2js.py. Throws when the embedded resource is missing or malformed.
    AXCCORE_API const alx::json_array& langref();

} // namespace core

#endif // ASCORE_H
