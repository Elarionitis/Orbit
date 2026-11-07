#include <gtest/gtest.h>

#include "client/text.h"

using namespace orbit::client;

TEST(Sanitize, LeavesPrintableTextAlone) {
    EXPECT_EQ(sanitize_for_terminal("hello, world"), "hello, world");
    EXPECT_EQ(sanitize_for_terminal("caf\xC3\xA9 \xF0\x9F\x9A\x80"),
              "caf\xC3\xA9 \xF0\x9F\x9A\x80");
}

TEST(Sanitize, RemovesTerminalControlSequences) {
    EXPECT_EQ(sanitize_for_terminal("\x1b[2Jwiped"), "[2Jwiped");
    EXPECT_EQ(sanitize_for_terminal("\x1b]0;evil title\x07"), "]0;evil title");
    EXPECT_EQ(sanitize_for_terminal("a\rb\nc\bd\x7F"), "abcd");
}

TEST(Sanitize, RemovesEightBitControlsEncodedAsUtf8) {
    EXPECT_EQ(sanitize_for_terminal("x\xC2\x9B"
                                    "31my"),
              "x31my");                                            // U+009B is CSI.
    EXPECT_EQ(sanitize_for_terminal("\xC2\xA0ok"), "\xC2\xA0ok");  // U+00A0 is not a control.
}

TEST(Sanitize, TurnsTabsIntoSpaces) {
    EXPECT_EQ(sanitize_for_terminal("a\tb"), "a b");
}

TEST(NickColour, IsStable) {
    EXPECT_EQ(nick_colour("ada"), nick_colour("ada"));
}

TEST(ParseCommand, RecognisesCommandsWithArguments) {
    const auto command = parse_command("/msg grace  hello there  friend");
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->name, "msg");
    EXPECT_EQ(command->first, "grace");
    EXPECT_EQ(command->rest, "hello there  friend");
}

TEST(ParseCommand, HandlesBareCommand) {
    const auto command = parse_command("/quit");
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->name, "quit");
    EXPECT_TRUE(command->first.empty());
    EXPECT_TRUE(command->rest.empty());
}

TEST(ParseCommand, TreatsOtherTextAsChat) {
    EXPECT_FALSE(parse_command("hello").has_value());
    EXPECT_FALSE(parse_command("").has_value());
    EXPECT_FALSE(parse_command("//not a command").has_value());
}

TEST(LineEditor, InsertsAtTheCursor) {
    LineEditor editor;
    editor.insert("helo");
    editor.move_left();
    editor.insert("l");
    EXPECT_EQ(editor.text(), "hello");
}

TEST(LineEditor, BackspaceRemovesWholeUtf8Characters) {
    LineEditor editor;
    editor.insert("a\xF0\x9F\x9A\x80");
    editor.backspace();
    EXPECT_EQ(editor.text(), "a");
    editor.backspace();
    editor.backspace();  // On an empty line this must be harmless.
    EXPECT_TRUE(editor.text().empty());
}

TEST(LineEditor, CursorMovesByWholeCharacters) {
    LineEditor editor;
    editor.insert("\xC3\xA9x");
    editor.move_home();
    editor.move_right();
    EXPECT_EQ(editor.cursor(), 2U);
    editor.move_left();
    EXPECT_EQ(editor.cursor(), 0U);
    editor.move_left();
    EXPECT_EQ(editor.cursor(), 0U);
}

TEST(LineEditor, DeleteForwardRemovesTheNextCharacter) {
    LineEditor editor;
    editor.insert("\xC3\xA9x");
    editor.move_home();
    editor.delete_forward();
    EXPECT_EQ(editor.text(), "x");
    EXPECT_EQ(editor.cursor(), 0U);
}

TEST(LineEditor, DeleteWordStopsAtTheWordBoundary) {
    LineEditor editor;
    editor.insert("one two  ");
    editor.delete_word();
    EXPECT_EQ(editor.text(), "one ");
}

TEST(LineEditor, HistoryWalksBackAndRestoresTheDraft) {
    LineEditor editor;
    editor.insert("first");
    editor.submit();
    editor.insert("second");
    editor.submit();
    editor.insert("draft");

    editor.history_previous();
    EXPECT_EQ(editor.text(), "second");
    editor.history_previous();
    EXPECT_EQ(editor.text(), "first");
    editor.history_previous();
    EXPECT_EQ(editor.text(), "first");
    editor.history_next();
    EXPECT_EQ(editor.text(), "second");
    editor.history_next();
    EXPECT_EQ(editor.text(), "draft");
}

TEST(LineEditor, SubmitSkipsConsecutiveDuplicates) {
    LineEditor editor;
    editor.insert("same");
    editor.submit();
    editor.insert("same");
    editor.submit();
    editor.history_previous();
    editor.history_previous();
    EXPECT_EQ(editor.text(), "same");
    editor.history_next();
    EXPECT_TRUE(editor.text().empty());
}

TEST(DisplayWidth, CountsCodePoints) {
    EXPECT_EQ(display_width("abc"), 3U);
    EXPECT_EQ(display_width("\xC3\xA9\xF0\x9F\x9A\x80"), 2U);
}
