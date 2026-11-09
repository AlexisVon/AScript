#ifndef APROJECT_H
#define APROJECT_H

#include "global.h"

#include <string>
#include <vector>

// core::project — .ax project config (doc/ax.md v1): parse + config-driven run.
// .ax = light per-directory view: main (required) + engine params + search
// paths + optional hint JSON. Unknown keys ignored.

namespace core {
    namespace project {

        struct ax_config {
            std::string file; // .ax 文件路径(绝对)
            std::string dir; // .ax 所在目录(绝对)
            std::string name; // 工程显示名(可选;空 = .ax 文件名)
            std::string out; // 编译输出目录(相对 .ax 解析为绝对;空 = 每次选择)
            std::string main; // 起始 axc(相对 .ax 解析为绝对)
            alx::varmap engine; // 引擎参数,直喂 core::create
            std::vector<std::string> path; // 搜索路径(解析后绝对)
            std::vector<std::string> path_expr; // path 的原样表达式(供 axp 携带)
            std::vector<std::string> docpath; // 提示目录(解析后绝对;仅 IDE 补全使用)
            std::string hint; // hint JSON 文件(绝对;空 = 无)
            bool hook = true; // 是否启用调试 hook (默认 true)
            alx::uint_64 hook_freq = 1; // hook 频率 (默认 1)
        };

        // parse_ax — parse .ax; main missing/wrong type → false + _err.
        // Unknown keys ignored; wrong-typed known keys ignored (lenient for
        // hand-written files). Relative out/main/path/hint resolved against _dir.
        bool AXCCORE_API parse_ax(const std::string& _path, ax_config& _out, std::string& _err);

        /**
         * \brief Expand a library path expression
         *
         * Absolute paths pass through; a leading {script} names the running script/product
         * directory (at compile time: the .ax directory), {exe} the host executable's directory;
         * any other relative path is resolved against _script_dir.
         */
        std::string AXCCORE_API expand_path(const std::string& _expr, const std::string& _script_dir);

        /// The host's built-in library directory: <exe dir>/links
        std::vector<std::string> AXCCORE_API system_library_paths();

        /// The search_paths carried in an .axp hint (raw expressions); empty when absent/corrupt
        std::vector<std::string> AXCCORE_API axp_search_paths(const std::string& _axp, std::string& _err);

        // run — config-driven exec: create(engine) → set_search_paths → exec(main).
        // _hook wired when non-null (caller-owned); errors → stderr. Returns error code.
        int AXCCORE_API run(const ax_config& _ax, alx::script::hook_fn _hook = nullptr);

        // compile — config-driven compile: engine.compile(main, cmps, embed, hint).
        // _out empty → <ax名>.axp in the .ax "out" dir when set, else next to the
        // .ax; _cmps = compress ast+hint (default per ax.md). _debug: -1 keep the
        // .ax engine value, 0 off, 1 on (position markers). Failure → false + _err
        // (engine compile errors collected).
        bool AXCCORE_API compile(const ax_config& _ax, bool _embed, const std::string& _out,
                                 std::string& _err, bool _cmps = true, int _debug = -1,
                                 const std::vector<std::string>& _extra_paths = {});

        // compile_file — compile one .axc to _out. _project non-null → its engine
        // params and search paths apply (file inside that project); debug always
        // explicit. Failure → false + _err.
        bool AXCCORE_API compile_file(const std::string& _file, const std::string& _out, bool _debug,
                                      bool _embed, const ax_config* _project, std::string& _err,
                                      bool _cmps = true,
                                      const std::vector<std::string>& _extra_paths = {});

        // ── .axp read-only view (T10) ────────────────────────────────────
        // Raw texts from engine::unpack (E1) — the GUI shows them verbatim.
        struct axp_view {
            std::string main_text; // main program AST dump
            std::vector<std::pair<std::string, std::string>> modules; // module key → dump
            std::string info_text; // metadata as JSON text (pretty-printed)
            bool has_hint = false; // hint key present in the binary
            std::string hint_text; // raw hint JSON
        };

        // unpack_axp — read .axp, engine::unpack (E1), extract the raw texts.
        // Failure (unreadable / not a compiled binary) → false + _err.
        bool AXCCORE_API unpack_axp(const std::string& _path, axp_view& _out, std::string& _err);

    } // namespace project
} // namespace core

#endif // APROJECT_H
