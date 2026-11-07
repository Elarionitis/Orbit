#pragma once

#include <termios.h>

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include "client/chat_client.h"
#include "client/text.h"
#include "net/event_loop.h"

namespace orbit::client {

// The interactive chat front end. Keyboard input and the network share one event
// loop, so the whole client is a single thread with no locks. When stdin is not a
// terminal it reads plain lines instead, which makes it scriptable.
class TerminalApp final : public net::IoHandler {
public:
    TerminalApp(net::EventLoop& loop, ChatClient::Options options, std::string initial_room);
    ~TerminalApp() override;

    TerminalApp(const TerminalApp&) = delete;
    TerminalApp& operator=(const TerminalApp&) = delete;

    void start();
    bool failed() const { return m_failed; }

    void on_io(bool readable, bool writable, bool failed) override;

private:
    void enter_raw_mode();
    void leave_raw_mode();

    void on_key_bytes(std::string_view bytes);
    void on_escape_sequence(std::string_view sequence);
    void on_line(std::string line);
    void on_command(const Command& command);
    void quit();
    void flush_queued_lines();

    void print(const std::string& line);
    void notice(const std::string& text);
    void redraw_prompt();
    std::string prompt() const;

    net::EventLoop& m_loop;
    ChatClient m_client;
    std::string m_nick;
    std::string m_initial_room;
    std::string m_current_room;
    LineEditor m_editor;

    bool m_interactive;
    bool m_raw_mode = false;
    bool m_failed = false;
    bool m_quitting = false;
    bool m_stdin_registered = false;
    termios m_saved_termios{};
    std::string m_pending_input;  // Partial escape sequence, UTF-8 sequence, or line.

    // Scripted input can arrive before the connection is up, so it waits for the
    // first join rather than being refused.
    std::deque<std::string> m_queued_lines;
    bool m_ready_for_input = false;
    bool m_quit_after_queue = false;
};

}  // namespace orbit::client
