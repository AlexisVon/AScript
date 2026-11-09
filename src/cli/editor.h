#pragma once
#ifndef _WIN32

#    include <string>
#    include <termios.h>
#    include <vector>

namespace cli {
    class editor {
    public:
        editor();
        ~editor();
        // Returns false on EOF (Ctrl+D on empty line), true otherwise
        bool readline(std::string& out);

    private:
        bool read_char(char& c);
        void handle_escape();
        void refresh();
        void move_cursor(int delta);
        void set_cursor(size_t pos);
        void history_prev();
        void history_next();

        termios m_orig;
        std::string m_buf;
        size_t m_pos = 0;

        std::vector<std::string> m_hist;
        int m_hist_idx = -1;
        std::string m_saved; // saved input when browsing history
    };
}

#endif // _WIN32
