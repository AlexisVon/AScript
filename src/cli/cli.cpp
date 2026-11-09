#include "cli.h"
#include "editor.h"

#include "afile.h"
#include "core/core.h"
#include "core/extend.h"
#include "core/project.h"

#include "aregex_ex.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace cli {

    // mode — what to do with the input source
    enum class mode {
        run, // execute (source or .axp, engine auto-detects)
        compile, // compile to .axp (-c)
        ast, // print AST dump (-j)
        fmt, // pretty print to stdout (-f)
    };

    // cli_val — value to printable string (core::utils::repr, kept as the local name)
    static std::string cli_val(const alx::variant& _v) {
        return core::utils::repr(_v);
    }

    // usage_error — option parse failure: message + usage on stderr, exit code 2
    static int usage_error(const std::string& _msg) {
        std::cerr << "AScript: " << _msg << "\n"
                  << "usage: AScript [options] <file.axc>\n"
                  << "  --overflow-check  --max-stack N  --max-vecfill N  --debug  --no-debug\n"
                  << "  -c  -o <out>  -x  -j  -f  --embed  -e <code>  -i  -p <dir>  --hook  --no-hook\n";
        return 2;
    }

    // parse_num — strict unsigned decimal: digits only, no sign/space
    static bool parse_num(const char* _s, size_t& _out) {
        if (!_s || !*_s) return false;
        for (const char* p = _s; *p; p++) {
            if (*p < '0' || *p > '9') return false;
        }
        errno = 0;
        char* end = nullptr;
        unsigned long long v = std::strtoull(_s, &end, 10);
        if (errno != 0) return false;
        _out = static_cast<size_t>(v);
        return true;
    }

    // out_path — default compile output: same dir, extension replaced by .axp
    static std::string out_path(const std::string& _in) {
        size_t slash = _in.find_last_of("/\\");
        size_t dot = _in.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
            return _in + ".axp";
        return _in.substr(0, dot) + ".axp";
    }

    // read_file — whole file into string, empty on failure
    static std::string read_file(const std::string& _path) {
        std::ifstream f(_path, std::ios::binary);
        if (!f) return {};
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // cli_hook — trap breakpoint printer (on unless --no-hook)
    static bool cli_hook(alx::script::hook_info& info) {
        if (info.type != alx::script::hook_event::trap) return true;
        auto& m = info.info.to<alx::varmap>();
        auto& here = m.value("here").to<alx::varmap>();
        std::string file = here.value("file").to<std::string>();
        if (file.empty()) file = "?";
        std::cout << "[trap] " << file << ":" << here.value("row").to<alx::int_64>()
                  << ":" << here.value("col").to<alx::int_64>();
        if (m.contain("args")) std::cout << "  arg=" << cli_val(m.value("args"));
        std::cout << "\n";
        for (alx::uint_64 i = info.wkfm_size(); i > 0; i--) {
            alx::uint_64 idx = i - 1;
            // entity prefix: import alias "p1::", else "[point.axc] " like the engine trace
            std::string ent;
            if (const void* e = info.wkfm_eptr(idx)) {
                std::string alias = info.wken_name(e); // m_alias: import alias, empty on root
                if (!alias.empty()) {
                    ent = alias + "::";
                } else {
                    std::string f = info.wken_file(e);
                    // top-level = root entity (dir path or empty); modules are .axc files
                    bool top = f.empty() || f == "::";
                    if (!top) {
                        top = alx::file_info(f).is_dir();
                    }
                    if (top) {
                        ent = "::";
                    } else {
                        auto pos = f.rfind('/');
                        ent = "[" + (pos != std::string::npos ? f.substr(pos + 1) : f) + "] ";
                    }
                }
            }
            std::cout << "    at " << ent << info.wkfm_func(idx);
            // frame locals — lean here; full engine view lands in GUI
            auto keys = info.wkfm_data_keys(idx);
            if (!keys.empty()) {
                std::cout << "  {";
                for (size_t k = 0; k < keys.size(); k++) {
                    if (k) std::cout << ", ";
                    std::cout << keys[k] << "="
                              << cli_val(*info.wkfm_data_cptr(idx, keys[k].c_str()));
                }
                std::cout << "}";
            }
            std::cout << "\n";
        }
        return true;
    }

    // make_engine — create engine, wire error signals, hook and paths
    static alx::script::engine* make_engine(const alx::varmap& _cfg, bool _hook,
                                            const std::vector<std::string>& _paths) {
        auto* eng = core::create(core::varmap_to_config(_cfg));
        // engine reports errors via signals — host must wire them to stderr
        eng->on_cerr.connect([](const std::string& _s) {
            std::cerr << _s;
            if (_s.empty() || _s.back() != '\n') std::cerr << "\n";
        });
        eng->on_cmpl.connect([](const alx::script::compile_error& _e) {
            std::cerr << _e.loc.row << ":" << _e.loc.col << ": " << _e.msg << "\n";
        });
        if (_hook) eng->set_hook(cli_hook, nullptr, 0);
        std::vector<std::string> paths = _paths;
        for (const auto& p : core::project::system_library_paths()) paths.push_back(p);
        eng->set_search_paths(std::list<std::string>(paths.begin(), paths.end()));
        return eng;
    }

    // run_repl — interactive loop: buffer lines, run on balanced braces + trailing ;/}
    static void run_repl(const alx::varmap& _cfg, bool _hook, const std::vector<std::string>& _paths) {
        std::cout << "AScript REPL. Type 'exit' to quit.\n";
        auto* eng = make_engine(_cfg, _hook, _paths);
#ifndef _WIN32
        editor ed;
#endif
        std::string line, buffer;
        while (true) {
            if (buffer.empty())
                std::cout << ">>> " << std::flush;
            else
                std::cout << "... " << std::flush;
#ifdef _WIN32
            if (!std::getline(std::cin, line)) break;
#else
            if (!ed.readline(line)) break;
#endif
            if (line == "exit") break;
            buffer += line;
            int brace = 0;
            for (char c : buffer) {
                if (c == '{') brace++;
                if (c == '}') brace--;
            }
            if (brace <= 0 && !buffer.empty() && (buffer.back() == ';' || buffer.back() == '}')) {
                alx::bytes src(buffer);
                auto res = eng->exec(alx::bytes_view(src), "");
                if (res.error == alx::script::error_type::NoError && !res.value.null())
                    std::cout << cli_val(res.value) << "\n";
                buffer.clear();
            } else {
                buffer += "\n";
            }
        }
        core::destroy(eng);
        std::cout << "\n";
    }

    int impl(int _argc, char** _argv) {
        std::string file, out, inline_code;
        std::vector<std::string> paths;
        bool overflow = false, hook = true, embed = false, interactive = false, ret = false;
        int debug = -1; // -1 unset (.ax value kept), 0 off, 1 on
        size_t max_stack = 0, max_vecfill = 0;
        mode m = mode::run;

        for (int i = 1; i < _argc; i++) {
            const char* a = _argv[i];
            // next — consume the following arg as this option's value
            auto next = [&]() -> const char* {
                if (i + 1 >= _argc) return nullptr;
                return _argv[++i];
            };
            // pick — select mode, reject conflicts
            auto pick = [&](mode _m) {
                if (m != mode::run && m != _m) return usage_error("conflicting modes (-c/-x/-j/-f)");
                m = _m;
                return 0;
            };
            if (std::strcmp(a, "--overflow-check") == 0) {
                overflow = true;
            } else if (std::strcmp(a, "--ret") == 0) {
                ret = true;
            } else if (std::strcmp(a, "--debug") == 0) {
                debug = 1;
            } else if (std::strcmp(a, "--no-debug") == 0) {
                debug = 0;
            } else if (std::strcmp(a, "--hook") == 0) {
                hook = true;
            } else if (std::strcmp(a, "--no-hook") == 0) {
                hook = false;
            } else if (std::strcmp(a, "--max-stack") == 0) {
                const char* v = next();
                if (!v || !parse_num(v, max_stack)) return usage_error("--max-stack needs a number");
            } else if (std::strcmp(a, "--max-vecfill") == 0) {
                const char* v = next();
                if (!v || !parse_num(v, max_vecfill)) return usage_error("--max-vecfill needs a number");
            } else if (std::strcmp(a, "-c") == 0) {
                if (int r = pick(mode::compile)) return r;
            } else if (std::strcmp(a, "-x") == 0) {
                if (int r = pick(mode::run)) return r;
            } else if (std::strcmp(a, "-j") == 0) {
                if (int r = pick(mode::ast)) return r;
            } else if (std::strcmp(a, "-f") == 0 || std::strcmp(a, "--fmt") == 0) {
                if (int r = pick(mode::fmt)) return r;
            } else if (std::strcmp(a, "-o") == 0) {
                const char* v = next();
                if (!v) return usage_error("-o needs a file path");
                out = v;
            } else if (std::strcmp(a, "--embed") == 0) {
                embed = true;
            } else if (std::strcmp(a, "-e") == 0) {
                const char* v = next();
                if (!v) return usage_error("-e needs code");
                inline_code = v;
            } else if (std::strcmp(a, "-i") == 0) {
                interactive = true;
            } else if (std::strcmp(a, "-p") == 0) {
                const char* v = next();
                if (!v) return usage_error("-p needs a directory");
                paths.push_back(v);
            } else if (a[0] == '-') {
                return usage_error(std::string("unknown option: ") + a);
            } else if (!file.empty()) {
                return usage_error("multiple input files");
            } else {
                file = a;
            }
        }
        if (file.empty() && inline_code.empty() && !interactive) return usage_error("no input file");
        if (!file.empty() && !inline_code.empty()) return usage_error("both -e and file");
        if (interactive && (m != mode::run || !file.empty() || !inline_code.empty()))
            return usage_error("-i conflicts with file/-e/other modes");
        if (m != mode::compile) {
            if (!out.empty()) return usage_error("-o only with -c");
            if (embed) return usage_error("--embed only with -c");
        }

        alx::varmap cfg;
        if (overflow) cfg["overflow_check"] = alx::variant(true);
        if (debug >= 0) cfg["debug_enable"] = alx::variant(debug == 1);
        if (max_stack) cfg["max_stack"] = alx::variant(alx::uint_64(max_stack));
        if (max_vecfill) cfg["max_vecfill"] = alx::variant(alx::uint_64(max_vecfill));

        // .ax project entry: run / compile only; -j/-f reject
        const bool is_ax = file.size() > 3 && file.compare(file.size() - 3, 3, ".ax") == 0;
        if (is_ax) {
            if (m == mode::ast || m == mode::fmt)
                return usage_error(".ax project files support run and -c only");
            core::project::ax_config ax;
            std::string err;
            if (!core::project::parse_ax(file, ax, err)) {
                std::cerr << "AScript: " << err << "\n";
                return 2;
            }
            // CLI flags override .ax engine keys; -p appends path
            if (overflow) ax.engine["overflow_check"] = alx::variant(true);
            if (debug >= 0) ax.engine["debug_enable"] = alx::variant(debug == 1);
            if (max_stack) ax.engine["max_stack"] = alx::variant(alx::uint_64(max_stack));
            if (max_vecfill) ax.engine["max_vecfill"] = alx::variant(alx::uint_64(max_vecfill));
            for (const std::string& p : paths) ax.path.push_back(p);
            if (m == mode::compile) {
                std::string cerr;
                if (!core::project::compile(ax, embed, out, cerr)) {
                    std::cerr << "AScript: " << cerr << "\n";
                    return 16;
                }
                return 0;
            }
            return core::project::run(ax, hook ? cli_hook : nullptr);
        }

        if (m == mode::ast) {
            std::string src = inline_code.empty() ? read_file(file) : inline_code;
            if (src.empty()) {
                std::cerr << "AScript: cannot read " << file << "\n";
                return 2;
            }
            alx::bytes b(src);
            std::cout << alx::script::engine::prtast(alx::bytes_view(b)) << "\n";
            return 0;
        }

        if (m == mode::fmt) {
            std::string src = inline_code.empty() ? read_file(file) : inline_code;
            if (src.empty()) {
                std::cerr << "AScript: cannot read " << file << "\n";
                return 2;
            }
            alx::bytes b(src);
            std::cout << alx::script::engine::prtfmt(alx::bytes_view(b)).to_string();
            return 0;
        }

        if (interactive) {
            run_repl(cfg, hook, paths);
            return 0;
        }

        // an .axp carries its search paths in the hint: apply them before the run
        if (m != mode::compile && !file.empty() && file.size() > 4 &&
            file.compare(file.size() - 4, 4, ".axp") == 0) {
            std::string hint_err;
            const size_t slash = file.find_last_of("/\\");
            const std::string dir = slash == std::string::npos ? "." : file.substr(0, slash);
            for (const auto& p : core::project::axp_search_paths(file, hint_err))
                paths.push_back(core::project::expand_path(p, dir));
        }
        auto* eng = make_engine(cfg, hook && m != mode::compile, paths);

        if (m == mode::compile) {
            alx::bytes bin = inline_code.empty()
                                 ? eng->compile(file, false, embed)
                                 : eng->compile(alx::bytes_view(alx::bytes(inline_code)), "", false, embed);
            if (bin.empty()) {
                std::cerr << "AScript: compile failed\n";
                return 16;
            }
            std::string target = out.empty() ? (inline_code.empty() ? out_path(file) : "out.axp") : out;
            const std::filesystem::path parent = std::filesystem::path(target).parent_path();
            if (!parent.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(parent, ec);
                if (ec) {
                    std::cerr << "AScript: cannot create " << parent << ": " << ec.message()
                              << "\n";
                    return 2;
                }
            }
            std::ofstream f(target, std::ios::binary);
            if (!f) {
                std::cerr << "AScript: cannot write " << target << "\n";
                return 2;
            }
            f.write(reinterpret_cast<const char*>(bin.data()), bin.size());
            return 0;
        }

        alx::script::engine::result res;
        if (!inline_code.empty()) {
            alx::bytes src(inline_code);
            res = eng->exec(alx::bytes_view(src), "");
        } else {
            res = eng->exec(file);
        }
        // --ret: print the exec return value with a marker, so the return value
        // stays distinguishable from whatever the script itself printed
        if (ret && res.error == alx::script::error_type::NoError && !res.value.null())
            std::cout << "[ret] " << cli_val(res.value) << "\n";
        core::destroy(eng);
        return static_cast<int>(res.error);
    }

    void help() {
        std::cout << "AScript - command line mode\n"
                  << "usage: AScript [options] <file.axc>\n"
                  << "  -h                    show global help\n"
                  << "  -v                    print version\n"
                  << "  --doc [regex]         print extend manual (regex filters names)\n"
                  << "  --toc [regex]         list extend function names (regex filters names)\n"
                  << "  --lang [regex]        print the language reference (regex filters chapters)\n"
                  << "  -c                    compile to .axp (default: <file>.axp or the .ax out dir)\n"
                  << "  -o <out>              compile output path (with -c)\n"
                  << "  -x                    execute (source or .axp; default mode)\n"
                  << "  -j                    print AST dump\n"
                  << "  --embed               self-contained compile (with -c)\n"
                  << "  -e <code>             execute inline code (with -c/-j too)\n"
                  << "  -i                    interactive REPL\n"
                  << "  --overflow-check      enable arithmetic overflow check\n"
                  << "  --max-stack N         max stack depth (0 = unlimited)\n"
                  << "  --max-vecfill N       max fill count per statement\n"
                  << "  -p <dir>              add import search path (repeatable)\n"
                  << "  --debug               record statement positions (line numbers in traces)\n"
                  << "  --no-debug            disable statement position recording (default)\n"
                  << "  --ret                 print the script's return value\n"
                  << "  --hook                enable trap breakpoint printing (default)\n"
                  << "  --no-hook             disable trap breakpoint printing\n";
    }

    namespace {

        // plain_text — inline markers stripped: `code` keeps its content, **bold** loses the stars
        std::string plain_text(const std::string& _s) {
            std::string out;
            size_t i = 0;
            while (i < _s.size()) {
                if (_s[i] == '`') {
                    const size_t j = _s.find('`', i + 1);
                    if (j == std::string::npos) {
                        out += _s.substr(i);
                        break;
                    }
                    out += _s.substr(i + 1, j - i - 1);
                    i = j + 1;
                    continue;
                }
                if (_s.compare(i, 2, "**") == 0) {
                    const size_t j = _s.find("**", i + 2);
                    if (j == std::string::npos) {
                        out += _s.substr(i);
                        break;
                    }
                    out += plain_text(_s.substr(i + 2, j - i - 2));
                    i = j + 2;
                    continue;
                }
                out += _s[i++];
            }
            return out;
        }

        // display_width — UTF-8 display cells; CJK and fullwidth count as two
        size_t display_width(const std::string& _s) {
            size_t w = 0;
            for (size_t i = 0; i < _s.size();) {
                const unsigned char c = static_cast<unsigned char>(_s[i]);
                unsigned cp = c;
                size_t n = 1;
                if (c >= 0xF0) {
                    cp = c & 0x07;
                    n = 4;
                } else if (c >= 0xE0) {
                    cp = c & 0x0F;
                    n = 3;
                } else if (c >= 0xC0) {
                    cp = c & 0x1F;
                    n = 2;
                }
                for (size_t k = 1; k < n && i + k < _s.size(); ++k)
                    cp = (cp << 6) | (static_cast<unsigned char>(_s[i + k]) & 0x3F);
                i += n;
                const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                                  (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                                  (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                                  (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
                w += wide ? 2 : 1;
            }
            return w;
        }

        std::string pad_cell(const std::string& _s, size_t _width, const std::string& _align) {
            const size_t w = display_width(_s);
            if (w >= _width) return _s;
            const size_t gap = _width - w;
            if (_align == "right") return std::string(gap, ' ') + _s;
            if (_align == "center")
                return std::string(gap / 2, ' ') + _s + std::string(gap - gap / 2, ' ');
            return _s + std::string(gap, ' ');
        }

        std::string table_text(const alx::json_object& _b) {
            const auto& rows = _b.value("rows").to<alx::json_array>();
            size_t cols = 0;
            for (const alx::json_value* r : rows)
                cols = std::max(cols, r->to<alx::json_array>().size());
            if (!cols) return std::string();

            std::vector<std::string> align(cols, "left");
            if (_b.contain("align")) {
                size_t c = 0;
                for (const alx::json_value* a : _b.value("align").to<alx::json_array>())
                    if (c < cols) align[c++] = a->to<std::string>();
            }

            std::vector<std::vector<std::string>> cells;
            std::vector<size_t> width(cols, 0);
            for (const alx::json_value* r : rows) {
                const auto& src = r->to<alx::json_array>();
                std::vector<std::string> row;
                for (size_t c = 0; c < cols; ++c)
                    row.push_back(c < src.size() ? plain_text(src[static_cast<int>(c)].to<std::string>())
                                                 : std::string());
                for (size_t c = 0; c < cols; ++c)
                    width[c] = std::max(width[c], display_width(row[c]));
                cells.push_back(std::move(row));
            }

            std::string out;
            for (size_t r = 0; r < cells.size(); ++r) {
                for (size_t c = 0; c < cols; ++c) {
                    if (c) out += "  ";
                    out += pad_cell(cells[r][c], width[c], align[c]);
                }
                out += '\n';
                if (r == 0) {
                    for (size_t c = 0; c < cols; ++c) {
                        if (c) out += "  ";
                        out += std::string(width[c], '-');
                    }
                    out += '\n';
                }
            }
            return out;
        }

        std::string block_text(const alx::json_object& _b) {
            const std::string type =
                _b.contain("type") ? _b.value("type").to<std::string>() : std::string();
            if (type == "heading") {
                long long level = _b.contain("level") ? _b.value("level").to_intg(1) : 1;
                if (level < 1 || level > 3) level = 1;
                return std::string(static_cast<size_t>(level), '#') + " " +
                       plain_text(_b.value("text").to<std::string>());
            }
            if (type == "paragraph") return plain_text(_b.value("text").to<std::string>());
            if (type == "quote") return "> " + plain_text(_b.value("text").to<std::string>());
            if (type == "code") {
                std::string out = "```";
                if (_b.contain("lang")) out += _b.value("lang").to<std::string>();
                out += '\n';
                for (const alx::json_value* line : _b.value("lines").to<alx::json_array>()) {
                    out += line->to<std::string>();
                    out += '\n';
                }
                return out + "```";
            }
            if (type == "table") return table_text(_b);
            if (type == "list") {
                const bool ordered = _b.contain("ordered") && _b.value("ordered").to_bool();
                std::string out;
                int n = 1;
                for (const alx::json_value* item : _b.value("items").to<alx::json_array>()) {
                    out += ordered ? std::to_string(n++) + ". " : std::string("- ");
                    out += plain_text(item->to<std::string>());
                    out += '\n';
                }
                if (!out.empty()) out.pop_back();
                return out;
            }
            return std::string();
        }

    } // namespace

    int print_langref(const std::string& _re) {
        alx::regex_ex re(_re);
        if (!_re.empty() && !re.is_valid()) {
            std::cerr << "invalid regex: " << _re << "\n";
            return 1;
        }
        const alx::json_array& blocks = core::langref();

        struct Section {
            int block;
            int end;
            std::string title;
        };
        struct Chapter {
            int begin;
            int end;
            std::string title;
            std::vector<Section> sections;
        };
        std::vector<Chapter> chapters;
        Chapter cur{0, 0, std::string(), {}};
        bool open = false;
        for (int i = 0; i < static_cast<int>(blocks.size()); ++i) {
            const auto& b = blocks[i].to<alx::json_object>();
            if (!b.contain("type") || b.value("type").to<std::string>() != "heading") continue;
            const long long level = b.contain("level") ? b.value("level").to_intg(1) : 1;
            const std::string title = plain_text(b.value("text").to<std::string>());
            if (level == 1) {
                if (open) {
                    cur.end = i;
                    chapters.push_back(std::move(cur));
                }
                cur = Chapter{i, 0, title, {}};
                open = true;
            } else if (open) {
                cur.sections.push_back({i, i, title});
            }
        }
        if (open) {
            cur.end = static_cast<int>(blocks.size());
            chapters.push_back(std::move(cur));
        }
        for (auto& ch : chapters)
            for (size_t s = 0; s < ch.sections.size(); ++s)
                ch.sections[s].end = s + 1 < ch.sections.size() ? ch.sections[s + 1].block : ch.end;

        auto print_range = [&blocks](int _begin, int _end) {
            for (int i = _begin; i < _end; ++i)
                std::cout << block_text(blocks[i].to<alx::json_object>()) << "\n\n";
        };

        for (const auto& ch : chapters) {
            if (_re.empty() || !re.find(ch.title).empty()) {
                print_range(ch.begin, ch.end);
                continue;
            }
            std::vector<const Section*> hits;
            for (const auto& s : ch.sections)
                if (!re.find(s.title).empty()) hits.push_back(&s);
            if (hits.empty()) continue;
            std::cout << block_text(blocks[ch.begin].to<alx::json_object>()) << "\n\n";
            for (const Section* s : hits) print_range(s->block, s->end);
        }
        return 0;
    }

} // namespace cli
