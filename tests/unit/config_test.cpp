#include "server/config.h"

#include <gtest/gtest.h>

#include <fstream>
#include <vector>

#include "support/temp_dir.h"

using namespace orbit::server;

namespace {
ParsedArgs parse(std::vector<const char*> args) {
    args.insert(args.begin(), "orbit_server");
    return parse_args(static_cast<int>(args.size()), args.data());
}
}  // namespace

TEST(Config, DefaultsAreValid) {
    const auto parsed = parse({});
    ASSERT_TRUE(parsed.config.has_value());
    EXPECT_EQ(parsed.config->port, 8080);
    EXPECT_FALSE(validate(*parsed.config).has_value());
}

TEST(Config, AcceptsSpaceAndEqualsForms) {
    const auto parsed = parse({"--port", "9000", "--host=0.0.0.0", "--threads=2"});
    ASSERT_TRUE(parsed.config.has_value());
    EXPECT_EQ(parsed.config->port, 9000);
    EXPECT_EQ(parsed.config->host, "0.0.0.0");
    EXPECT_EQ(parsed.config->threads, 2U);
}

TEST(Config, ParsesSyncPolicy) {
    EXPECT_EQ(parse({"--fsync", "always"}).config->sync_policy, orbit::storage::SyncPolicy::Always);
    EXPECT_EQ(parse({"--fsync", "none"}).config->sync_policy, orbit::storage::SyncPolicy::None);
    EXPECT_FALSE(parse({"--fsync", "sometimes"}).config.has_value());
}

TEST(Config, RejectsBadInput) {
    for (const auto& args : std::vector<std::vector<const char*>>{
             {"--port", "70000"},
             {"--port", "abc"},
             {"--port", "80x"},
             {"--port"},
             {"--threads", "0"},
             {"--no-such-flag", "1"},
         }) {
        const auto parsed = parse(args);
        EXPECT_FALSE(parsed.config.has_value()) << args[0];
        EXPECT_EQ(parsed.exit_code, 2);
        EXPECT_FALSE(parsed.message.empty());
    }
}

TEST(Config, HelpAndVersionExitCleanly) {
    for (const char* flag : {"--help", "--version"}) {
        const auto parsed = parse({flag});
        EXPECT_FALSE(parsed.config.has_value());
        EXPECT_EQ(parsed.exit_code, 0);
        EXPECT_FALSE(parsed.message.empty());
    }
}

TEST(Config, RejectsContradictorySettings) {
    EXPECT_FALSE(parse({"--tls-cert", "a.pem"}).config.has_value());
    EXPECT_FALSE(parse({"--max-frame-bytes", "1000", "--max-body-bytes", "900"}).config.has_value());
    EXPECT_FALSE(parse({"--heartbeat-ms", "5000", "--idle-timeout-ms", "5000"}).config.has_value());
}

TEST(Config, ReadsTheTokenFromAFile) {
    orbit::testing::TempDir dir;
    const auto path = dir.path() / "token";
    std::ofstream(path) << "hunter2\n";
    const auto parsed = parse({"--auth-token-file", path.c_str()});
    ASSERT_TRUE(parsed.config.has_value());
    EXPECT_EQ(parsed.config->auth_token, "hunter2");

    EXPECT_FALSE(parse({"--auth-token-file", "/nonexistent"}).config.has_value());
}
