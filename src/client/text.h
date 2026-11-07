#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace orbit::client {

// Removes everything a terminal would interpret: C0 and C1 control characters,
// which covers ESC. Message bodies come from other users, and without this anyone
// in a room could rewrite your screen or set your window title.
std::string sanitize_for_terminal(std::string_view text);

// A stable colour per nickname, as an ANSI 256-colour index.
int nick_colour(std::string_view nick);

struct Command {
    std::string name;
    std::string first;  // First argument, if any.
    std::string rest;   // Everything after the first argument.
};

// Parses "/name first rest of line". Returns nullopt for ordinary chat text. A
// doubled slash escapes: "//etc" is the chat message "/etc".
std::optional<Command> parse_command(std::string_view line);

// A single-line editor with UTF-8 aware cursor movement and input history. It
// knows nothing about terminals, which keeps it testable.
class LineEditor {
public:
    const std::string& text() const { return m_text; }
    std::size_t cursor() const { return m_cursor; }

    void insert(std::string_view bytes);
    void backspace();
    void delete_forward();
    void delete_word();
    void clear();
    void move_left();
    void move_right();
    void move_home() { m_cursor = 0; }
    void move_end() { m_cursor = m_text.size(); }

    void history_previous();
    void history_next();

    // Returns the line, records it in history, and resets for the next one.
    std::string submit();

private:
    std::string m_text;
    std::size_t m_cursor = 0;
    std::vector<std::string> m_history;
    std::size_t m_history_position = 0;
    std::string m_draft;
};

// Display columns from the start of `text` to byte `offset`. Each code point counts
// as one column; this ignores double-width characters.
std::size_t display_width(std::string_view text);

}  // namespace orbit::client
