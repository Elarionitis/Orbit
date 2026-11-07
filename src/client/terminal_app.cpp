#include "client/terminal_app.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <format>
#include <utility>

#include "protocol/messages.h"

namespace orbit::client {
namespace {

using namespace protocol;

constexpr std::string_view kHelp =
    "commands: /join <room>  /leave [room]  /switch <room>  /who [room]  /rooms\n"
    "          /msg <nick> <text>  /help  /quit";

constexpr std::string_view kDim = "\x1b[2m";
constexpr std::string_view kBold = "\x1b[1m";
constexpr std::string_view kReset = "\x1b[0m";

std::string clock_time(std::uint64_t timestamp_ms) {
    const auto seconds = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm local{};
    localtime_r(&seconds, &local);
    std::array<char, 16> text{};
    std::strftime(text.data(), text.size(), "%H:%M", &local);
    return text.data();
}

std::string coloured(std::string_view nick) {
    return std::format("\x1b[38;5;{}m{}{}", nick_colour(nick), nick, kReset);
}

std::size_t terminal_width() {
    winsize size{};
    if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0) {
        return size.ws_col;
    }
    return 80;
}

void write_stdout(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

// Length of a UTF-8 sequence from its lead byte.
std::size_t utf8_length(unsigned char lead) {
    if (lead >= 0xF0)
        return 4;
    if (lead >= 0xE0)
        return 3;
    if (lead >= 0xC0)
        return 2;
    return 1;
}

}  // namespace

TerminalApp::TerminalApp(net::EventLoop& loop, const ChatClient::Options& options,
                         std::string initial_room)
    : m_loop(loop),
      m_client(
          loop, options,
          ChatClient::Events{
              .on_state =
                  [this](ChatClient::State state, const std::string& detail) {
                      switch (state) {
                          case ChatClient::State::Online:
                              notice("connected");
                              break;
                          case ChatClient::State::Backoff:
                              notice(detail);
                              break;
                          case ChatClient::State::Stopped:
                              if (!detail.empty()) {
                                  notice(detail);
                                  m_failed = true;
                                  quit();
                              }
                              break;
                          default:
                              break;
                      }
                      redraw_prompt();
                  },
              .on_joined =
                  [this](const Joined& joined, std::uint64_t missed) {
                      if (m_current_room.empty()) {
                          m_current_room = joined.room;
                      }
                      notice(
                          std::format("joined #{} ({} online)", joined.room, joined.member_count));
                      if (missed > 0) {
                          notice(std::format("{} older messages in #{} are no longer available",
                                             missed, joined.room));
                      }
                      flush_queued_lines();
                  },
              .on_left =
                  [this](const Left& left) {
                      notice(std::format("left #{}", left.room));
                      if (m_current_room == left.room) {
                          m_current_room.clear();
                      }
                      redraw_prompt();
                  },
              .on_message =
                  [this](const Message& message, bool history) {
                      const std::string room =
                          message.room == m_current_room ? "" : "#" + message.room + " ";
                      print(std::format("{}{} {}{}{}{}: {}", kDim, clock_time(message.timestamp_ms),
                                        room, kReset, history ? kDim : "", coloured(message.nick),
                                        sanitize_for_terminal(message.body)));
                  },
              .on_presence =
                  [this](const Presence& presence) {
                      if (presence.nick != m_nick) {
                          notice(std::format("{} {} #{}", presence.nick,
                                             presence.joined ? "joined" : "left", presence.room));
                      }
                  },
              .on_direct =
                  [this](const DirectMessage& message) {
                      print(std::format("{}{}{} {}[dm]{} {}: {}", kDim,
                                        clock_time(message.timestamp_ms), kReset, kBold, kReset,
                                        coloured(message.from),
                                        sanitize_for_terminal(message.body)));
                  },
              .on_who =
                  [this](const WhoReply& reply) {
                      std::string names;
                      for (const auto& nick : reply.nicks) {
                          names += nick + " ";
                      }
                      notice(std::format("in #{}: {}", reply.room, names));
                  },
              .on_rooms =
                  [this](const RoomList& list) {
                      if (list.rooms.empty()) {
                          notice("no rooms yet");
                      }
                      for (const auto& room : list.rooms) {
                          notice(std::format("#{} ({} online)", room.name, room.member_count));
                      }
                  },
              .on_error =
                  [this](const Error& error) {
                      notice(std::format("error: {} {}", error_name(error.code),
                                         sanitize_for_terminal(error.detail)));
                  },
          }),
      m_nick(options.nick),
      m_initial_room(std::move(initial_room)),
      m_interactive(::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0) {}

TerminalApp::~TerminalApp() {
    if (m_stdin_registered) {
        m_loop.remove(STDIN_FILENO);
    }
    leave_raw_mode();
}

void TerminalApp::start() {
    enter_raw_mode();
    net::set_nonblocking(STDIN_FILENO);
    m_loop.add(STDIN_FILENO, net::kInterestRead, this);
    m_stdin_registered = true;

    notice(std::format("orbit {} - type /help for commands", ORBIT_VERSION));
    if (!m_initial_room.empty()) {
        m_client.join(m_initial_room);
    }
    m_client.start();
    redraw_prompt();
}

void TerminalApp::enter_raw_mode() {
    if (!m_interactive || ::tcgetattr(STDIN_FILENO, &m_saved_termios) != 0) {
        return;
    }
    termios raw = m_saved_termios;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
        m_raw_mode = true;
    }
}

void TerminalApp::leave_raw_mode() {
    if (m_raw_mode) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_saved_termios);
        m_raw_mode = false;
        write_stdout("\r\x1b[2K");
    }
}

void TerminalApp::on_io(bool, bool, bool) {
    std::array<char, 1024> buffer;
    const ssize_t count = ::read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count == 0) {
        m_loop.remove(STDIN_FILENO);
        m_stdin_registered = false;
        if (m_queued_lines.empty()) {
            quit();
        } else {
            m_quit_after_queue = true;
        }
        return;
    }
    if (count < 0) {
        return;
    }
    const std::string_view bytes(buffer.data(), static_cast<std::size_t>(count));
    if (m_interactive) {
        on_key_bytes(bytes);
        return;
    }
    m_pending_input.append(bytes);
    std::size_t newline = 0;
    while ((newline = m_pending_input.find('\n')) != std::string::npos) {
        std::string line = m_pending_input.substr(0, newline);
        m_pending_input.erase(0, newline + 1);
        if (m_ready_for_input) {
            on_line(std::move(line));
        } else {
            m_queued_lines.push_back(std::move(line));
        }
    }
}

void TerminalApp::flush_queued_lines() {
    m_ready_for_input = true;
    while (!m_queued_lines.empty()) {
        std::string line = std::move(m_queued_lines.front());
        m_queued_lines.pop_front();
        on_line(std::move(line));
    }
    if (m_quit_after_queue) {
        quit();
    }
}

void TerminalApp::on_key_bytes(std::string_view bytes) {
    m_pending_input.append(bytes);
    std::string_view input = m_pending_input;

    while (!input.empty()) {
        const auto key = static_cast<unsigned char>(input[0]);
        if (key == 0x1B) {  // Escape sequence: ESC [ ... final-byte.
            if (input.size() < 2) {
                break;
            }
            if (input[1] != '[') {
                input.remove_prefix(2);
                continue;
            }
            std::size_t end = 2;
            while (end < input.size() && (input[end] < 0x40 || input[end] > 0x7E)) {
                ++end;
            }
            if (end == input.size()) {
                break;
            }
            on_escape_sequence(input.substr(2, end - 1));
            input.remove_prefix(end + 1);
            continue;
        }
        if (key >= 0x80) {  // UTF-8 text: wait for the whole character.
            const std::size_t length = utf8_length(key);
            if (input.size() < length) {
                break;
            }
            m_editor.insert(input.substr(0, length));
            input.remove_prefix(length);
            continue;
        }
        input.remove_prefix(1);
        switch (key) {
            case '\r':
            case '\n': {
                std::string line = m_editor.submit();
                redraw_prompt();
                on_line(std::move(line));
                break;
            }
            case 0x03:  // Ctrl-C
                quit();
                return;
            case 0x04:  // Ctrl-D
                if (m_editor.text().empty()) {
                    quit();
                    return;
                }
                m_editor.delete_forward();
                break;
            case 0x7F:  // Backspace
            case 0x08:
                m_editor.backspace();
                break;
            case 0x01:  // Ctrl-A
                m_editor.move_home();
                break;
            case 0x05:  // Ctrl-E
                m_editor.move_end();
                break;
            case 0x15:  // Ctrl-U
                m_editor.clear();
                break;
            case 0x17:  // Ctrl-W
                m_editor.delete_word();
                break;
            default:
                if (key >= 0x20) {
                    m_editor.insert(std::string_view(reinterpret_cast<const char*>(&key), 1));
                }
                break;
        }
    }
    m_pending_input.erase(0, m_pending_input.size() - input.size());
    redraw_prompt();
}

void TerminalApp::on_escape_sequence(std::string_view sequence) {
    if (sequence == "A") {
        m_editor.history_previous();
    } else if (sequence == "B") {
        m_editor.history_next();
    } else if (sequence == "C") {
        m_editor.move_right();
    } else if (sequence == "D") {
        m_editor.move_left();
    } else if (sequence == "H" || sequence == "1~") {
        m_editor.move_home();
    } else if (sequence == "F" || sequence == "4~") {
        m_editor.move_end();
    } else if (sequence == "3~") {
        m_editor.delete_forward();
    }
}

void TerminalApp::on_line(std::string line) {
    if (line.empty()) {
        return;
    }
    if (const auto command = parse_command(line)) {
        on_command(*command);
        return;
    }
    if (line.starts_with("//")) {
        line.erase(0, 1);
    }
    if (m_current_room.empty()) {
        notice("you are not in a room: /join <room>");
    } else if (!m_client.publish(m_current_room, line)) {
        notice("not connected; message not sent");
    }
}

void TerminalApp::on_command(const Command& command) {
    const std::string& room = command.first.empty() ? m_current_room : command.first;
    if (command.name == "quit" || command.name == "q") {
        quit();
    } else if (command.name == "help") {
        print(std::string(kHelp));
    } else if (command.name == "join" || command.name == "j") {
        if (command.first.empty()) {
            notice("usage: /join <room>");
        } else {
            m_current_room = command.first;
            m_client.join(command.first);
        }
    } else if (command.name == "leave") {
        m_client.leave(room);
    } else if (command.name == "switch") {
        m_current_room = command.first;
        redraw_prompt();
    } else if (command.name == "who") {
        m_client.who(room);
    } else if (command.name == "rooms") {
        m_client.list_rooms();
    } else if (command.name == "msg") {
        if (command.first.empty() || command.rest.empty()) {
            notice("usage: /msg <nick> <text>");
        } else if (m_client.direct(command.first, command.rest)) {
            print(std::format("{}[dm to {}]{} {}", kDim, command.first, kReset, command.rest));
        } else {
            notice("not connected; message not sent");
        }
    } else {
        notice(std::format("unknown command /{}: try /help", command.name));
    }
}

void TerminalApp::quit() {
    if (m_quitting) {
        return;
    }
    m_quitting = true;
    if (m_client.state() != ChatClient::State::Stopped) {
        m_client.stop();
    }
    // Deferred work runs in order, so this lands after the final flush.
    m_loop.defer([this] { m_loop.stop(); });
}

void TerminalApp::print(const std::string& line) {
    if (!m_interactive) {
        write_stdout(line + "\n");
        return;
    }
    // Clear the prompt, print above it, and put the prompt back.
    std::string text = "\r\x1b[2K";
    for (const char c : line) {
        if (c == '\n') {
            text += "\r\n";
        } else {
            text += c;
        }
    }
    text += "\r\n";
    write_stdout(text);
    redraw_prompt();
}

void TerminalApp::notice(const std::string& text) {
    print(std::format("{}-- {}{}", kDim, sanitize_for_terminal(text), kReset));
}

std::string TerminalApp::prompt() const {
    const bool online = m_client.state() == ChatClient::State::Online;
    const std::string where = m_current_room.empty() ? "no room" : "#" + m_current_room;
    return std::format("{}{}{}{}> ", online ? kBold : kDim, where, online ? "" : " (offline)",
                       kReset);
}

void TerminalApp::redraw_prompt() {
    if (!m_interactive) {
        return;
    }
    const std::string shown_prompt = prompt();
    const std::string plain_prompt =
        (m_current_room.empty() ? "no room" : "#" + m_current_room) +
        (m_client.state() == ChatClient::State::Online ? "" : " (offline)") + "> ";
    const std::size_t prompt_width = display_width(plain_prompt);
    const std::size_t width = terminal_width();
    const std::string_view text = m_editor.text();
    const std::size_t cursor_column = display_width(text.substr(0, m_editor.cursor()));

    // Keep the cursor in view by scrolling long lines horizontally.
    const std::size_t room = width > prompt_width + 1 ? width - prompt_width - 1 : 1;
    const std::size_t skip_columns = cursor_column >= room ? cursor_column - room + 1 : 0;

    std::size_t start = 0;
    for (std::size_t skipped = 0; skipped < skip_columns && start < text.size();) {
        start += utf8_length(static_cast<unsigned char>(text[start]));
        ++skipped;
    }
    std::size_t end = start;
    for (std::size_t shown = 0; shown < room && end < text.size(); ++shown) {
        end += utf8_length(static_cast<unsigned char>(text[end]));
    }
    end = std::min(end, text.size());

    write_stdout(std::format("\r\x1b[2K{}{}\r\x1b[{}C", shown_prompt,
                             text.substr(start, end - start),
                             prompt_width + cursor_column - skip_columns));
}

}  // namespace orbit::client
