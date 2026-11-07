#include <gtest/gtest.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "support/server_fixture.h"

using namespace orbit::protocol;
using orbit::testing::ServerTest;
using orbit::testing::TempDir;
using orbit::testing::TestClient;

namespace {

std::optional<std::string> metric_line(const std::string& text, const std::string& name) {
    const auto start = text.find("\n" + name + " ");
    if (start == std::string::npos) {
        return std::nullopt;
    }
    const auto end = text.find('\n', start + 1);
    return text.substr(start + 1, end - start - 1);
}

}  // namespace

// ---- Handshake ----

TEST_F(ServerTest, WelcomesAValidClient) {
    start_server();
    TestClient client(port());
    client.send(Hello{kProtocolVersion, "ada", ""});
    const auto welcome = client.expect<Welcome>();
    EXPECT_NE(welcome.session_id, 0U);
    EXPECT_EQ(welcome.heartbeat_ms, config.heartbeat_ms);
    EXPECT_EQ(welcome.max_frame_size, config.max_frame_size);
    EXPECT_EQ(welcome.server_version, ORBIT_VERSION);
}

TEST_F(ServerTest, RefusesAnUnsupportedProtocolVersion) {
    start_server();
    TestClient client(port());
    client.send(Hello{999, "ada", ""});
    EXPECT_EQ(client.expect<Error>().code, ErrorCode::UnsupportedVersion);
    EXPECT_TRUE(client.wait_closed());
}

TEST_F(ServerTest, RefusesAnInvalidNickname) {
    start_server();
    TestClient client(port());
    client.send(Hello{kProtocolVersion, "bad nick\x1b[31m", ""});
    EXPECT_EQ(client.expect<Error>().code, ErrorCode::InvalidName);
}

TEST_F(ServerTest, NicknamesAreUniqueIgnoringCase) {
    start_server();
    auto first = login("ada");
    TestClient second(port());
    second.send(Hello{kProtocolVersion, "ADA", ""});
    EXPECT_EQ(second.expect<Error>().code, ErrorCode::NicknameTaken);
}

TEST_F(ServerTest, NicknameIsReleasedOnDisconnect) {
    start_server();
    {
        auto first = login("ada");
        first.disconnect();
    }
    ASSERT_TRUE(eventually([&] {
        TestClient retry(port());
        retry.send(Hello{kProtocolVersion, "ada", ""});
        auto reply = retry.receive();
        return reply.has_value() && std::holds_alternative<Welcome>(*reply);
    }));
}

TEST_F(ServerTest, RequiresTheConfiguredToken) {
    config.auth_token = "s3cret";
    start_server();

    TestClient wrong(port());
    wrong.send(Hello{kProtocolVersion, "ada", "guess"});
    EXPECT_EQ(wrong.expect<Error>().code, ErrorCode::AuthenticationFailed);

    TestClient missing(port());
    missing.send(Hello{kProtocolVersion, "ada", ""});
    EXPECT_EQ(missing.expect<Error>().code, ErrorCode::AuthenticationFailed);

    auto right = TestClient::login(port(), "ada", "s3cret");
}

TEST_F(ServerTest, CommandsBeforeHelloAreAViolation) {
    start_server();
    TestClient client(port());
    client.send(Join{"general", 0});
    EXPECT_EQ(client.expect<Error>().code, ErrorCode::ProtocolViolation);
    EXPECT_TRUE(client.wait_closed());
}

TEST_F(ServerTest, SecondHelloIsAViolation) {
    start_server();
    auto client = login("ada");
    client.send(Hello{kProtocolVersion, "other", ""});
    EXPECT_EQ(client.expect<Error>().code, ErrorCode::ProtocolViolation);
}

// ---- Rooms and messages ----

TEST_F(ServerTest, DeliversMessagesToEveryMemberIncludingTheSender) {
    start_server();
    auto ada = login("ada");
    auto grace = login("grace");
    ada.join("general");
    grace.join("general");

    ada.send(Publish{42, "general", "hello"});
    const auto ack = ada.wait_for<Ack>();
    ASSERT_TRUE(ack.has_value());
    EXPECT_EQ(ack->correlation, 42U);
    EXPECT_EQ(ack->seq, 1U);

    for (auto* client : {&grace}) {
        const auto message = client->wait_for<Message>();
        ASSERT_TRUE(message.has_value());
        EXPECT_EQ(message->room, "general");
        EXPECT_EQ(message->nick, "ada");
        EXPECT_EQ(message->body, "hello");
        EXPECT_EQ(message->seq, 1U);
    }
}

TEST_F(ServerTest, MessagesStayInsideTheirRoom) {
    start_server();
    auto ada = login("ada");
    auto grace = login("grace");
    ada.join("alpha");
    grace.join("beta");

    ada.send(Publish{1, "alpha", "for alpha only"});
    ASSERT_TRUE(ada.wait_for<Ack>().has_value());
    EXPECT_FALSE(grace.wait_for<Message>(100).has_value());
}

TEST_F(ServerTest, RefusesPublishingToARoomNotJoined) {
    start_server();
    auto ada = login("ada");
    auto grace = login("grace");
    ada.join("general");

    grace.send(Publish{9, "general", "let me in"});
    const auto error = grace.expect<Error>();
    EXPECT_EQ(error.code, ErrorCode::NotInRoom);
    EXPECT_EQ(error.correlation, 9U);
}

TEST_F(ServerTest, LeavingStopsDelivery) {
    start_server();
    auto ada = login("ada");
    auto grace = login("grace");
    ada.join("general");
    grace.join("general");

    grace.send(Leave{"general"});
    ASSERT_TRUE(grace.wait_for<Left>().has_value());

    ada.send(Publish{1, "general", "after leaving"});
    ASSERT_TRUE(ada.wait_for<Ack>().has_value());
    EXPECT_FALSE(grace.wait_for<Message>(100).has_value());
}

TEST_F(ServerTest, AnnouncesPresence) {
    start_server();
    auto ada = login("ada");
    ada.join("general");
    ASSERT_TRUE(ada.wait_for<Presence>().has_value());  // The client's own arrival.

    auto grace = login("grace");
    grace.join("general");

    const auto joined = ada.wait_for<Presence>();
    ASSERT_TRUE(joined.has_value());
    EXPECT_EQ(joined->nick, "grace");
    EXPECT_TRUE(joined->joined);

    grace.disconnect();
    const auto left = ada.wait_for<Presence>();
    ASSERT_TRUE(left.has_value());
    EXPECT_EQ(left->nick, "grace");
    EXPECT_FALSE(left->joined);
}

TEST_F(ServerTest, ListsMembersAndRooms) {
    start_server();
    auto ada = login("ada");
    auto grace = login("grace");
    ada.join("general");
    grace.join("general");
    grace.join("random");

    ada.send(Who{"general"});
    const auto who = ada.wait_for<WhoReply>();
    ASSERT_TRUE(who.has_value());
    EXPECT_EQ(who->nicks, (std::vector<std::string>{"ada", "grace"}));

    ada.send(ListRooms{});
    const auto rooms = ada.wait_for<RoomList>();
    ASSERT_TRUE(rooms.has_value());
    ASSERT_EQ(rooms->rooms.size(), 2U);
    EXPECT_EQ(rooms->rooms[0], (RoomInfo{"general", 2}));
    EXPECT_EQ(rooms->rooms[1], (RoomInfo{"random", 1}));
}

TEST_F(ServerTest, DirectMessagesReachOnlyTheTarget) {
    start_server();
    auto ada = login("ada");
    auto grace = login("grace");
    auto eve = login("eve");

    ada.send(Direct{5, "GRACE", "psst"});  // Lookup ignores case.
    const auto received = grace.wait_for<DirectMessage>();
    ASSERT_TRUE(received.has_value());
    EXPECT_EQ(received->from, "ada");
    EXPECT_EQ(received->body, "psst");
    EXPECT_EQ(ada.expect<Ack>().correlation, 5U);
    EXPECT_FALSE(eve.wait_for<DirectMessage>(100).has_value());
}

TEST_F(ServerTest, DirectMessageToNobodyIsAnError) {
    start_server();
    auto ada = login("ada");
    ada.send(Direct{5, "ghost", "hello?"});
    const auto error = ada.expect<Error>();
    EXPECT_EQ(error.code, ErrorCode::UnknownUser);
    EXPECT_EQ(error.correlation, 5U);
}

TEST_F(ServerTest, PingIsAnswered) {
    start_server();
    auto client = login("ada");
    client.send(Ping{31337});
    EXPECT_EQ(client.expect<Pong>().nonce, 31337U);
}

// ---- The ordering guarantee ----

// The central claim of the design. Many publishers on different shards write to
// one room at once, and every member must observe the identical sequence with no
// gaps and no duplicates.
TEST_F(ServerTest, AllMembersSeeOneTotalOrderUnderConcurrency) {
    constexpr int kPublishers = 8;
    constexpr int kPerPublisher = 500;
    constexpr int kTotal = kPublishers * kPerPublisher;
    constexpr int kReaders = 6;

    start_server();

    std::vector<TestClient> readers;
    for (int i = 0; i < kReaders; ++i) {
        readers.push_back(login("reader" + std::to_string(i)));
        readers.back().join("firehose");
    }
    std::vector<TestClient> publishers;
    for (int i = 0; i < kPublishers; ++i) {
        publishers.push_back(login("writer" + std::to_string(i)));
        publishers.back().join("firehose");
    }

    std::vector<std::thread> writers;
    for (int p = 0; p < kPublishers; ++p) {
        writers.emplace_back([&, p] {
            for (int i = 0; i < kPerPublisher; ++i) {
                publishers[static_cast<std::size_t>(p)].send(
                    Publish{static_cast<std::uint64_t>(i), "firehose",
                            std::to_string(p) + ":" + std::to_string(i)});
            }
        });
    }

    std::vector<std::vector<std::string>> observed(kReaders);
    std::vector<std::thread> reader_threads;
    for (int r = 0; r < kReaders; ++r) {
        reader_threads.emplace_back([&, r] {
            std::uint64_t expected_seq = 1;
            auto& log = observed[static_cast<std::size_t>(r)];
            while (log.size() < kTotal) {
                const auto message = readers[static_cast<std::size_t>(r)].wait_for<Message>(10000);
                if (!message.has_value()) {
                    break;
                }
                EXPECT_EQ(message->seq, expected_seq++);
                log.push_back(message->body);
            }
        });
    }

    for (auto& writer : writers) {
        writer.join();
    }
    for (auto& reader : reader_threads) {
        reader.join();
    }

    ASSERT_EQ(observed[0].size(), static_cast<std::size_t>(kTotal));
    for (int r = 1; r < kReaders; ++r) {
        EXPECT_EQ(observed[static_cast<std::size_t>(r)], observed[0]) << "reader " << r;
    }

    // Each publisher's own messages must also keep their send order.
    std::map<std::string, int> last_index;
    for (const std::string& body : observed[0]) {
        const auto colon = body.find(':');
        const auto publisher = body.substr(0, colon);
        const int index = std::stoi(body.substr(colon + 1));
        const auto previous = last_index.find(publisher);
        if (previous != last_index.end()) {
            EXPECT_EQ(index, previous->second + 1);
        }
        last_index[publisher] = index;
    }
}

// A client joining during a burst must see every message exactly once: history up
// to the instant of joining, then the live stream, with no gap at the seam.
TEST_F(ServerTest, JoinHandsOverFromHistoryToLiveWithoutGapOrDuplicate) {
    constexpr int kMessages = 3000;
    config.history_messages = kMessages + 10;
    config.history_bytes = 4 * 1024 * 1024;
    config.max_queued_bytes = 32 * 1024 * 1024;
    start_server();

    auto writer = login("writer");
    writer.join("stream");

    std::atomic<bool> go{false};
    std::thread publisher([&] {
        for (int i = 0; i < kMessages; ++i) {
            writer.send(Publish{0, "stream", std::to_string(i)});
            if (i == kMessages / 3) {
                go = true;
            }
        }
    });

    while (!go) {
        std::this_thread::yield();
    }

    auto late = login("late");
    late.send(Join{"stream", 0});

    std::uint64_t expected = 1;
    while (expected <= kMessages) {
        const auto message = late.wait_for<Message>(10000);
        ASSERT_TRUE(message.has_value()) << "stalled before seq " << expected;
        ASSERT_EQ(message->seq, expected);
        ++expected;
    }
    publisher.join();
}

TEST_F(ServerTest, JoinReplaysHistory) {
    start_server();
    auto ada = login("ada");
    ada.join("general");
    for (int i = 1; i <= 5; ++i) {
        ada.send(Publish{0, "general", "m" + std::to_string(i)});
    }
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(ada.wait_for<Message>().has_value());
    }

    auto grace = login("grace");
    const auto joined = grace.join("general");
    EXPECT_EQ(joined.head_seq, 5U);
    EXPECT_EQ(joined.first_seq, 1U);
    for (int i = 1; i <= 5; ++i) {
        const auto message = grace.wait_for<Message>();
        ASSERT_TRUE(message.has_value());
        EXPECT_EQ(message->body, "m" + std::to_string(i));
    }
}

TEST_F(ServerTest, RejoiningCatchesUpFromTheLastSeenSequence) {
    start_server();
    auto ada = login("ada");
    ada.join("general");
    for (int i = 1; i <= 10; ++i) {
        ada.send(Publish{0, "general", std::to_string(i)});
    }
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(ada.wait_for<Message>().has_value());
    }

    auto grace = login("grace");
    grace.join("general", 7);
    for (std::uint64_t seq = 8; seq <= 10; ++seq) {
        const auto message = grace.wait_for<Message>();
        ASSERT_TRUE(message.has_value());
        EXPECT_EQ(message->seq, seq);
    }
    EXPECT_FALSE(grace.wait_for<Message>(100).has_value());
}

TEST_F(ServerTest, HistoryIsBoundedAndReportsWhereItStarts) {
    config.history_messages = 3;
    start_server();
    auto ada = login("ada");
    ada.join("general");
    for (int i = 1; i <= 10; ++i) {
        ada.send(Publish{0, "general", std::to_string(i)});
    }
    for (int i = 1; i <= 10; ++i) {
        ASSERT_TRUE(ada.wait_for<Message>().has_value());
    }

    auto grace = login("grace");
    const auto joined = grace.join("general");
    EXPECT_EQ(joined.head_seq, 10U);
    EXPECT_EQ(joined.first_seq, 8U);
    EXPECT_EQ(grace.wait_for<Message>()->seq, 8U);
}

// ---- Robustness against bad clients ----

TEST_F(ServerTest, DropsClientThatDeclaresAnOversizedFrame) {
    config.max_frame_size = 4096 + 512;
    config.max_body_size = 4096;
    start_server();
    auto client = login("ada");
    client.send_raw({0x7F, 0xFF, 0xFF, 0xFF});
    EXPECT_EQ(client.expect<Error>().code, ErrorCode::ProtocolViolation);
    EXPECT_TRUE(client.wait_closed());
}

TEST_F(ServerTest, DropsClientSendingGarbage) {
    start_server();
    auto client = login("ada");
    client.send_raw({0, 0, 0, 3, 0x7E, 1, 2});
    EXPECT_EQ(client.expect<Error>().code, ErrorCode::ProtocolViolation);
}

TEST_F(ServerTest, DropsClientSendingServerMessages) {
    start_server();
    auto client = login("ada");
    client.send(Ack{1, 2, 3});
    EXPECT_EQ(client.expect<Error>().code, ErrorCode::ProtocolViolation);
}

TEST_F(ServerTest, ReassemblesFramesDeliveredOneByteAtATime) {
    start_server();
    auto client = login("ada");
    for (const std::uint8_t byte : encode(Ping{7})) {
        client.send_raw({byte});
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(client.expect<Pong>().nonce, 7U);
}

TEST_F(ServerTest, HandlesManyFramesInOneRead) {
    start_server();
    auto client = login("ada");
    std::vector<std::uint8_t> burst;
    for (std::uint64_t i = 0; i < 1000; ++i) {
        const auto frame = encode(Ping{i});
        burst.insert(burst.end(), frame.begin(), frame.end());
    }
    client.send_raw(burst);
    for (std::uint64_t i = 0; i < 1000; ++i) {
        EXPECT_EQ(client.expect<Pong>().nonce, i);
    }
}

TEST_F(ServerTest, RejectsInvalidBodies) {
    config.max_body_size = 1024;
    start_server();
    auto client = login("ada");
    client.join("general");

    client.send(Publish{1, "general", ""});
    EXPECT_EQ(client.wait_for<Error>()->code, ErrorCode::InvalidMessage);
    client.send(Publish{2, "general", std::string(1025, 'x')});
    EXPECT_EQ(client.wait_for<Error>()->code, ErrorCode::InvalidMessage);
    client.send(Publish{3, "general", "bad \xFF utf8"});
    EXPECT_EQ(client.wait_for<Error>()->code, ErrorCode::InvalidMessage);
}

TEST_F(ServerTest, RateLimitsPublishers) {
    config.rate_per_second = 1;
    config.rate_burst = 5;
    start_server();
    auto client = login("ada");
    client.join("general");

    for (int i = 0; i < 20; ++i) {
        client.send(Publish{static_cast<std::uint64_t>(i), "general", "spam"});
    }
    int accepted = 0;
    int limited = 0;
    while (accepted + limited < 20) {
        auto message = client.receive();
        ASSERT_TRUE(message.has_value());
        if (std::holds_alternative<Ack>(*message)) {
            ++accepted;
        } else if (const auto* error = std::get_if<Error>(&*message)) {
            EXPECT_EQ(error->code, ErrorCode::RateLimited);
            ++limited;
        }
    }
    EXPECT_EQ(accepted, 5);
    EXPECT_EQ(limited, 15);
}

TEST_F(ServerTest, ClosesConnectionsThatNeverSendHello) {
    config.handshake_timeout_ms = 50;
    start_server();
    TestClient client(port());
    EXPECT_EQ(client.expect<Error>().code, ErrorCode::TimedOut);
    EXPECT_TRUE(client.wait_closed());
}

TEST_F(ServerTest, ClosesIdleSessionsButNotActiveOnes) {
    config.heartbeat_ms = 50;
    config.idle_timeout_ms = 200;
    start_server();
    auto idle = login("idle");
    auto active = login("active");

    const auto start = orbit::monotonic_ms();
    while (orbit::monotonic_ms() - start < 500) {
        active.send(Ping{1});
        ASSERT_TRUE(active.wait_for<Pong>().has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_EQ(idle.expect<Error>().code, ErrorCode::TimedOut);
}

TEST_F(ServerTest, RefusesConnectionsPastTheLimit) {
    config.max_connections = 2;
    start_server();
    auto first = login("one");
    auto second = login("two");
    TestClient third(port());
    EXPECT_EQ(third.expect<Error>().code, ErrorCode::ServerFull);
}

TEST_F(ServerTest, EnforcesRoomLimits) {
    config.max_rooms_per_session = 2;
    start_server();
    auto client = login("ada");
    client.join("one");
    client.join("two");
    client.send(Join{"three", 0});
    EXPECT_EQ(client.wait_for<Error>()->code, ErrorCode::Overloaded);
}

TEST_F(ServerTest, EnforcesTheGlobalRoomLimit) {
    config.max_rooms = 2;
    start_server();
    auto client = login("ada");
    client.join("one");
    client.join("two");
    client.send(Join{"three", 0});
    EXPECT_EQ(client.wait_for<Error>()->code, ErrorCode::Overloaded);
    client.join("one");  // Existing rooms stay joinable.
}

// One stalled client must not slow anybody else down, and must not grow the server
// without limit. It is cut off, and the healthy client never notices.
TEST_F(ServerTest, SlowConsumerIsDisconnectedWithoutHurtingOthers) {
    config.max_queued_bytes = 2 * 1024 * 1024;
    config.history_bytes = 1024 * 1024;
    start_server();

    auto writer = login("writer");
    auto healthy = login("healthy");
    auto stalled = login("stalled");
    writer.join("general");
    healthy.join("general");
    stalled.join("general");  // ...and then never reads again.

    const std::string body(8 * 1024, 'x');
    constexpr int kMessages = 3000;  // About 24 MB, well past socket buffers and the queue.

    std::thread publisher([&] {
        for (int i = 0; i < kMessages; ++i) {
            writer.send(Publish{0, "general", body});
        }
    });
    std::thread drain_writer([&] {
        // The writer's own echoes must be read or it would become slow itself.
        int seen = 0;
        while (seen < kMessages) {
            if (!writer.wait_for<Message>(10000).has_value()) {
                break;
            }
            ++seen;
        }
    });

    std::uint64_t expected = 1;
    while (expected <= kMessages) {
        const auto message = healthy.wait_for<Message>(10000);
        ASSERT_TRUE(message.has_value()) << "healthy client stalled at seq " << expected;
        ASSERT_EQ(message->seq, expected++);
    }
    publisher.join();
    drain_writer.join();

    const auto text = server->registry().render();
    EXPECT_NE(text.find("orbit_disconnects_total{reason=\"slow_consumer\"} 1"), std::string::npos);
}

TEST_F(ServerTest, GhostMembersAreCleanedUpWhenAClientVanishesMidJoin) {
    start_server();
    for (int i = 0; i < 50; ++i) {
        TestClient client = login("flaky" + std::to_string(i));
        client.send(Join{"general", 0});
        client.disconnect();
    }
    auto observer = login("observer");
    observer.join("general");
    ASSERT_TRUE(eventually([&] {
        observer.send(Who{"general"});
        const auto who = observer.wait_for<WhoReply>();
        return who.has_value() && who->nicks == std::vector<std::string>{"observer"};
    }));
}

TEST_F(ServerTest, SurvivesAStormOfConnectionsAndDisconnections) {
    start_server();
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 40; ++i) {
                auto client = TestClient::login(port(), std::format("s{}x{}", t, i));
                client.send(Join{"storm", 0});
                client.send(Publish{0, "storm", "hello"});
                if (i % 2 == 0) {
                    client.wait_for<Ack>(1000);
                }
                client.disconnect();
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    auto probe = login("probe");
    probe.send(Ping{1});
    EXPECT_TRUE(probe.wait_for<Pong>().has_value());
    EXPECT_TRUE(eventually([&] {
        return server->registry().render().find("orbit_connections_active 1\n") !=
               std::string::npos;
    }));
}

// ---- Lifecycle and operations ----

TEST_F(ServerTest, ShutdownNotifiesClients) {
    start_server();
    auto client = login("ada");
    std::thread stopper([&] { server->stop(); });
    EXPECT_EQ(client.wait_for<Error>()->code, ErrorCode::ServerShutdown);
    client.disconnect();
    stopper.join();
}

TEST_F(ServerTest, StopIsIdempotent) {
    start_server();
    server->stop();
    server->stop();
}

TEST_F(ServerTest, PortInUseFailsCleanly) {
    start_server();
    auto second_config = config;
    second_config.port = port();
    orbit::server::ChatServer second(second_config);
    EXPECT_THROW(second.start(), std::system_error);
}

TEST_F(ServerTest, ServesMetricsOverHttp) {
    config.metrics_port = 0;
    start_server();
    auto client = login("ada");
    client.join("general");
    client.send(Publish{1, "general", "counted"});
    ASSERT_TRUE(client.wait_for<Ack>().has_value());

    orbit::net::UniqueFd fd = orbit::net::connect_tcp("127.0.0.1", server->metrics_port());
    pollfd writable{fd.get(), POLLOUT, 0};
    ::poll(&writable, 1, 5000);
    const std::string request = "GET /metrics HTTP/1.0\r\n\r\n";
    ASSERT_GT(::send(fd.get(), request.data(), request.size(), orbit::net::send_flags()), 0);

    std::string response;
    for (;;) {
        pollfd readable{fd.get(), POLLIN, 0};
        ASSERT_GT(::poll(&readable, 1, 5000), 0);
        std::array<char, 4096> chunk{};
        const ssize_t count = ::recv(fd.get(), chunk.data(), chunk.size(), 0);
        if (count <= 0) {
            break;
        }
        response.append(chunk.data(), static_cast<std::size_t>(count));
    }

    EXPECT_TRUE(response.starts_with("HTTP/1.0 200 OK"));
    const auto published = metric_line(response, "orbit_messages_published_total");
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(*published, "orbit_messages_published_total 1");
    EXPECT_NE(response.find("orbit_publish_latency_seconds_count 1"), std::string::npos);
}

// ---- Persistence ----

class PersistenceTest : public ServerTest {
protected:
    PersistenceTest() {
        config.data_dir = dir.path();
        config.sync_policy = orbit::storage::SyncPolicy::None;
    }
    TempDir dir;
};

TEST_F(PersistenceTest, HistorySurvivesARestart) {
    start_server();
    {
        auto ada = login("ada");
        ada.join("general");
        for (int i = 1; i <= 20; ++i) {
            ada.send(Publish{0, "general", "m" + std::to_string(i)});
        }
        for (int i = 1; i <= 20; ++i) {
            ASSERT_TRUE(ada.wait_for<Ack>().has_value());
        }
    }
    server->stop();
    server.reset();

    start_server();
    EXPECT_EQ(server->recovery().records, 20U);

    auto grace = login("grace");
    const auto joined = grace.join("general");
    EXPECT_EQ(joined.head_seq, 20U);
    for (int i = 1; i <= 20; ++i) {
        const auto message = grace.wait_for<Message>();
        ASSERT_TRUE(message.has_value());
        EXPECT_EQ(message->body, "m" + std::to_string(i));
        EXPECT_EQ(message->nick, "ada");
    }
}

TEST_F(PersistenceTest, SequenceNumbersContinueAfterARestart) {
    start_server();
    {
        auto ada = login("ada");
        ada.join("general");
        ada.send(Publish{1, "general", "before"});
        ASSERT_EQ(ada.wait_for<Ack>()->seq, 1U);
    }
    server->stop();
    server.reset();

    start_server();
    auto ada = login("ada");
    ada.join("general");
    ada.send(Publish{2, "general", "after"});
    EXPECT_EQ(ada.wait_for<Ack>()->seq, 2U);
}

TEST_F(PersistenceTest, RestoredRoomsAreListed) {
    start_server();
    {
        auto ada = login("ada");
        ada.join("kept");
        ada.send(Publish{1, "kept", "x"});
        ASSERT_TRUE(ada.wait_for<Ack>().has_value());
    }
    server->stop();
    server.reset();

    start_server();
    auto ada = login("ada");
    ada.send(ListRooms{});
    const auto rooms = ada.wait_for<RoomList>();
    ASSERT_TRUE(rooms.has_value());
    ASSERT_EQ(rooms->rooms.size(), 1U);
    EXPECT_EQ(rooms->rooms[0].name, "kept");
}

TEST_F(PersistenceTest, AlwaysPolicyPreservesOrderUnderConcurrency) {
    config.sync_policy = orbit::storage::SyncPolicy::Always;
    start_server();

    constexpr int kWriters = 4;
    constexpr int kPerWriter = 100;
    auto reader = login("reader");
    reader.join("durable");

    std::vector<std::thread> threads;
    for (int w = 0; w < kWriters; ++w) {
        threads.emplace_back([&, w] {
            auto writer = TestClient::login(port(), "w" + std::to_string(w));
            writer.join("durable");
            for (int i = 0; i < kPerWriter; ++i) {
                writer.send(Publish{static_cast<std::uint64_t>(i), "durable", "x"});
            }
            for (int i = 0; i < kPerWriter; ++i) {
                EXPECT_TRUE(writer.wait_for<Ack>(20000).has_value());
            }
        });
    }

    for (std::uint64_t seq = 1; seq <= kWriters * kPerWriter; ++seq) {
        const auto message = reader.wait_for<Message>(20000);
        ASSERT_TRUE(message.has_value());
        ASSERT_EQ(message->seq, seq);
    }
    for (auto& thread : threads) {
        thread.join();
    }
}
