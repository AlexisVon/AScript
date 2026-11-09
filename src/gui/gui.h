#ifndef AUI_MAIN_IMPL_H
#define AUI_MAIN_IMPL_H

namespace gui {

    // GUI-mode entry (argv[1] == "-ui"). Opens the main window; file/dir
    // arguments are conceptual (editor / project tree) and not yet implemented.
    int impl(int _argc, char** _argv);


    // help — print usage
    void help();

} // namespace gui

#endif // AUI_MAIN_IMPL_H
