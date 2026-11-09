#include "cli.h"
#include "core.h"

#include <cstring>
#include <iostream>

// CLI entry point:
//   (no args)   -> show usage
//   -v          -> print version
//   -h          -> show help
//   --doc [re]  -> dump extend doc JSON, optional regex filter
//   --toc [re]  -> list extend function names only (--doc minus the manual)
//   --lang [re] -> print the language reference as plain text, optional regex filter
//   else        -> execute script

int main(int _argc, char** _argv) {
    if (_argc <= 1) {
        cli::help();
        return 0;
    }
    if (std::strcmp(_argv[1], "-v") == 0) {
        std::cout << core::version() << "\n";
        return 0;
    }
    for (int i = 1; i < _argc; i++) {
        if (std::strcmp(_argv[i], "-h") == 0) {
            cli::help();
            return 0;
        }
        if (std::strcmp(_argv[i], "--doc") == 0) {
            bool has_re = i + 1 < _argc && _argv[i + 1][0] != '-';
            const alx::json_object docs =
                has_re ? core::document(_argv[i + 1]) : core::document();
            std::cout << alx::json_doc::to_string(alx::json_value(docs), false) << "\n";
            return 0;
        }
        if (std::strcmp(_argv[i], "--toc") == 0) {
            bool has_re = i + 1 < _argc && _argv[i + 1][0] != '-';
            const alx::json_object docs =
                has_re ? core::document(_argv[i + 1]) : core::document();
            if (docs.contain("functions")) {
                const auto& fns = docs.value("functions").to<alx::json_object>();
                for (auto it = fns.begin(); it != fns.end(); ++it)
                    std::cout << "$" << it.key() << "\n";
            }
            return 0;
        }
        if (std::strcmp(_argv[i], "--lang") == 0) {
            const bool has_re = i + 1 < _argc && _argv[i + 1][0] != '-';
            return cli::print_langref(has_re ? _argv[i + 1] : std::string());
        }
    }
    return cli::impl(_argc, _argv);
}
