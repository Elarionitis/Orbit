#include "client/text.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace orbit::client {
namespace {

bool is_continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0U) == 0x80U;
}

}  // namespace

std::string sanitize_for_terminal(std::string_view text) {
    std::string clean;
    clean.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == '\t') {
            clean.push_back(' ');
        } else if (byte < 0x20 || byte == 0x7F) {
            continue;
        } else if (byte == 0xC2 && i + 1 < text.size() &&
                   static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
                   static_cast<unsigned char>(text[i + 1]) <= 0x9F) {
            ++i;  // C1 controls, U+0080 to U+009F, encoded in UTF-8.
        } else {
            clean.push_back(text[i]);
        }
    }
    return clean;
}

int nick_colour(std::string_view nick) {
    // Mid-brightness colours that stay readable on light and dark backgrounds.
    static constexpr std::array<int, 12> kPalette = {39, 41,  74,  79,  99,  105,
                                                     135, 141, 166, 172, 178, 203};
    std::uint32_t hash = 2166136261U;
    for (const char c : nick) {
        hash = (hash ^ static_cast<std::uint8_t>(c)) * 16777619U;
    }
    return kPalette[hash % kPalette.size()];
}

std::optional<Command> parse_command(std::string_view line) {
    if (line.empty() || line[0] != '/' || line.starts_with("//")) {
        return std::nullopt;
    }
    const auto next_word = [](std::string_view& input) {
        const std::size_t start = input.find_first_not_of(' ');
        if (start == std::string_view::npos) {
            input = {};
            return std::string_view{};
        }
        input.remove_prefix(start);
        const std::size_t end = std::min(input.find(' '), input.size());
        const std::string_view word = input.substr(0, end);
        input.remove_prefix(end);
        return word;
    };

    std::string_view remaining = line.substr(1);
    Command command;
    command.name = next_word(remaining);
    command.first = next_word(remaining);
    const std::size_t start = remaining.find_first_not_of(' ');
    if (start != std::string_view::npos) {
        command.rest = remaining.substr(start);
    }
    return command;
}

void LineEditor::insert(std::string_view bytes) {
    m_text.insert(m_cursor, bytes);
    m_cursor += bytes.size();
}

void LineEditor::backspace() {
    if (m_cursor == 0) {
        return;
    }
    const std::size_t end = m_cursor;
    move_left();
    m_text.erase(m_cursor, end - m_cursor);
}

void LineEditor::delete_forward() {
    if (m_cursor < m_text.size()) {
        const std::size_t start = m_cursor;
        move_right();
        m_text.erase(start, m_cursor - start);
        m_cursor = start;
    }
}

void LineEditor::delete_word() {
    std::size_t start = m_cursor;
    while (start > 0 && m_text[start - 1] == ' ') {
        --start;
    }
    while (start > 0 && m_text[start - 1] != ' ') {
        --start;
    }
    m_text.erase(start, m_cursor - start);
    m_cursor = start;
}

void LineEditor::clear() {
    m_text.clear();
    m_cursor = 0;
}

void LineEditor::move_left() {
    if (m_cursor == 0) {
        return;
    }
    --m_cursor;
    while (m_cursor > 0 && is_continuation(m_text[m_cursor])) {
        --m_cursor;
    }
}

void LineEditor::move_right() {
    if (m_cursor >= m_text.size()) {
        return;
    }
    ++m_cursor;
    while (m_cursor < m_text.size() && is_continuation(m_text[m_cursor])) {
        ++m_cursor;
    }
}

void LineEditor::history_previous() {
    if (m_history_position == 0) {
        return;
    }
    if (m_history_position == m_history.size()) {
        m_draft = m_text;
    }
    --m_history_position;
    m_text = m_history[m_history_position];
    m_cursor = m_text.size();
}

void LineEditor::history_next() {
    if (m_history_position >= m_history.size()) {
        return;
    }
    ++m_history_position;
    m_text = m_history_position == m_history.size() ? m_draft : m_history[m_history_position];
    m_cursor = m_text.size();
}

std::string LineEditor::submit() {
    std::string line = std::move(m_text);
    clear();
    if (!line.empty() && (m_history.empty() || m_history.back() != line)) {
        m_history.push_back(line);
    }
    m_history_position = m_history.size();
    m_draft.clear();
    return line;
}

std::size_t display_width(std::string_view text) {
    return static_cast<std::size_t>(
        std::count_if(text.begin(), text.end(), [](char c) { return !is_continuation(c); }));
}

}  // namespace orbit::client
