#include "editor.h"
#ifndef _WIN32

#    include <poll.h>
#    include <unistd.h>

cli::editor::editor() {
    tcgetattr(STDIN_FILENO, &m_orig);
    termios raw = m_orig;
    raw.c_lflag &= ~(ICANON | ECHO | IEXTEN);
    raw.c_iflag &= ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

cli::editor::~editor() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_orig);
}

bool cli::editor::read_char(char& c) {
    return ::read(STDIN_FILENO, &c, 1) == 1;
}

bool cli::editor::readline(std::string& out) {
    m_buf.clear();
    m_pos = 0;

    for (;;) {
        char c;
        if (!read_char(c)) return false;

        // Enter
        if (c == '\r' || c == '\n') {
            if (c == '\r') {
                struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
                if (poll(&pfd, 1, 10) > 0) {
                    char n;
                    ::read(STDIN_FILENO, &n, 1);
                }
            }
            write(STDOUT_FILENO, "\r\n", 2);
            break;
        }

        // Escape sequence
        if (c == '\033') {
            handle_escape();
            continue;
        }

        // Backspace
        if (c == 127 || c == '\b') {
            if (m_pos > 0) {
                m_buf.erase(m_pos - 1, 1);
                m_pos--;
                refresh();
            }
            continue;
        }

        // Ctrl+D on empty line -> EOF
        if (c == 4 && m_buf.empty()) return false;

        // Ctrl+C / Ctrl+Z — let signal handle it
        if (c == 3 || c == 26) continue;

        // Ctrl+A -> Home
        if (c == 1) {
            set_cursor(0);
            continue;
        }
        // Ctrl+E -> End
        if (c == 5) {
            set_cursor(m_buf.size());
            continue;
        }
        // Ctrl+K -> kill to end of line
        if (c == 11) {
            m_buf.erase(m_pos);
            refresh();
            continue;
        }
        // Ctrl+W -> kill word backward
        if (c == 23) {
            while (m_pos > 0 && m_buf[m_pos - 1] == ' ') {
                m_buf.erase(m_pos - 1, 1);
                m_pos--;
            }
            while (m_pos > 0 && m_buf[m_pos - 1] != ' ') {
                m_buf.erase(m_pos - 1, 1);
                m_pos--;
            }
            refresh();
            continue;
        }

        // Printable character
        if (c >= 32 && c < 127) {
            m_buf.insert(m_buf.begin() + m_pos, c);
            m_pos++;
            refresh();
        }
    }

    // Add to history (skip duplicates and empty)
    if (!m_buf.empty() && (m_hist.empty() || m_buf != m_hist.back()))
        m_hist.push_back(m_buf);
    m_hist_idx = -1;

    out = m_buf;
    return true;
}

void cli::editor::handle_escape() {
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    if (poll(&pfd, 1, 50) <= 0) return; // lone ESC, ignore

    char c;
    if (::read(STDIN_FILENO, &c, 1) != 1) return;

    // SS3: ESC O <letter>
    if (c == 'O') {
        if (::read(STDIN_FILENO, &c, 1) != 1) return;
        switch (c) {
        case 'A': history_prev(); return;
        case 'B': history_next(); return;
        case 'C': set_cursor(m_pos + 1); return;
        case 'D': set_cursor(m_pos > 0 ? m_pos - 1 : 0); return;
        case 'H': set_cursor(0); return;
        case 'F': set_cursor(m_buf.size()); return;
        }
        return;
    }

    // Not CSI
    if (c != '[') return;

    // Read CSI parameter bytes + final byte
    char csi[16] = {0};
    csi[0] = c; // '['
    int len = 1;
    while (len < 15) {
        if (::read(STDIN_FILENO, &c, 1) != 1) return;
        csi[len++] = c;
        if (c >= 0x40 && c <= 0x7E) break; // final byte reached
    }

    // len >= 2: '[' + final byte (e.g. "[A", "[H")
    // len >= 3: '[' + param + final (e.g. "[3~", "[1~")
    char final = csi[len - 1];

    if (len == 2) {
        // Single-byte CSI (no parameters)
        switch (final) {
        case 'A': history_prev(); return;
        case 'B': history_next(); return;
        case 'C': set_cursor(m_pos + 1); return;
        case 'D': set_cursor(m_pos > 0 ? m_pos - 1 : 0); return;
        case 'H': set_cursor(0); return;
        case 'F': set_cursor(m_buf.size()); return;
        }
    } else if (len >= 3 && final == '~') {
        // <param>~
        if (csi[1] == '3') {
            if (m_pos < m_buf.size()) {
                m_buf.erase(m_pos, 1);
                refresh();
            }
            return;
        } else if (csi[1] == '1') {
            set_cursor(0);
            return;
        } else if (csi[1] == '4') {
            set_cursor(m_buf.size());
            return;
        }
    }
    // Unrecognized sequence — ignore
}

void cli::editor::refresh() {
    std::string out = "\r\033[K";
    if (!m_buf.empty()) out += m_buf;
    if (m_pos < m_buf.size())
        out += "\033[" + std::to_string(m_buf.size() - m_pos) + "D";
    write(STDOUT_FILENO, out.c_str(), out.size());
}

void cli::editor::set_cursor(size_t pos) {
    if (pos > m_buf.size()) pos = m_buf.size();
    if (pos == m_pos) return;
    int diff = (int) pos - (int) m_pos;
    m_pos = pos;
    std::string seq = diff > 0
                          ? "\033[" + std::to_string(diff) + "C"
                          : "\033[" + std::to_string(-diff) + "D";
    write(STDOUT_FILENO, seq.c_str(), seq.size());
}

void cli::editor::history_prev() {
    if (m_hist.empty()) return;
    if (m_hist_idx == -1) {
        m_saved = m_buf;
        m_hist_idx = (int) m_hist.size() - 1;
    } else if (m_hist_idx > 0) {
        m_hist_idx--;
    }
    m_buf = m_hist[m_hist_idx];
    m_pos = m_buf.size();
    refresh();
}

void cli::editor::history_next() {
    if (m_hist_idx == -1) return;
    if (m_hist_idx < (int) m_hist.size() - 1) {
        m_hist_idx++;
        m_buf = m_hist[m_hist_idx];
    } else {
        m_hist_idx = -1;
        m_buf = m_saved;
    }
    m_pos = m_buf.size();
    refresh();
}

#endif // _WIN32
