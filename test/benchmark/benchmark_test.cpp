#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>
#include <sys/socket.h>

struct user_t {
    uint32_t id;
};

struct conn_ptr_t {
    user_t* user;
    int fd;
    bool authenticated;
};

struct conn_embedded_t {
    user_t user;
    int fd;
    bool authenticated;
};

/**
 * Creates a connection table of pointers, where each pointer points 
 * to a connection on the heap.
 */
static std::vector<conn_ptr_t*> make_indirect_table(size_t N) {
    std::vector<conn_ptr_t*> table;
    table.reserve(N);
    for (size_t i = 0; i < N; i++) {
        auto* conn = new conn_ptr_t();
        conn->user = new user_t();
        conn->user->id = i;
        conn->fd = i;
        conn->authenticated = true;
        table.push_back(conn);
    }
    return table;
}

/**
 * Create a table of connection objects, where each one is allocated on the heap.
 */
static std::vector<conn_ptr_t> make_direct_non_embedded_table(size_t N) {
    std::vector<conn_ptr_t> table;
    table.reserve(N);
    for (size_t i = 0; i < N; i++) {
        table[i].user = new user_t();
        table[i].user->id = i;
        table[i].fd = i;
        table[i].authenticated = true;
    }
    return table;
}

/**
 * Creates a connection table of N authenticated connections. 
 * 
 * @note This connection table is a table of structs rather than a table
 * of pointers. Also each connection a full user.
 */
static std::vector<conn_embedded_t> make_direct_table(size_t N) {
    std::vector<conn_embedded_t> table;
    table.reserve(N);

    for (size_t i = 0; i < N; i++) {
        conn_embedded_t conn{};
        conn.user.id = i;
        conn.fd = i;
        conn.authenticated = true;

        table.push_back(conn);
    }

    return table;
}

// --------------------------------------------------
// Benchmark 1:
// Traversal locality
//
// Hypothesis:
// vector<T> traverses faster than vector<T*>
// due to better cache locality.
// --------------------------------------------------

static void BM_Traversal_Indirect(benchmark::State& state) {
    
    // NOTE: Use state.range(i) to grab the ith argument.
    size_t N = state.range(0);
    auto table = make_indirect_table(N);
    for (auto _ : state) {
        uint64_t sum = 0;
        for (size_t i = 0; i < N; i++) {
            sum += table[i]->fd;
        }
        benchmark::DoNotOptimize(sum);
    }
    for (auto* conn : table) {
        delete conn->user;
        delete conn;
    }
}

static void BM_Traversal_Direct(benchmark::State& state) {
    size_t N = state.range(0);
    auto table = make_direct_table(N);
    for (auto _ : state) {
        uint64_t sum = 0;
        for (size_t i = 0; i < N; i++) {
            sum += table[i].fd;
        }
        benchmark::DoNotOptimize(sum);
    }
}

// --------------------------------------------------
// Benchmark 2:
// Pointer chasing
//
// Hypothesis:
// Indirect user access causes additional
// cache misses.
// --------------------------------------------------

static void BM_UserAccess_Indirect(benchmark::State& state) {
    size_t N = state.range(0);
    auto table = make_indirect_table(N);
    for (auto _ : state) {
        uint64_t sum = 0;
        for (size_t i = 0; i < N; i++) {
            sum += table[i]->user->id;
        }
        benchmark::DoNotOptimize(sum);
    }
    for (auto* conn : table) {
        delete conn->user;
        delete conn;
    }
}

static void BM_UserAccess_Direct(benchmark::State& state) {
    size_t N = state.range(0);
    auto table = make_direct_table(N);
    for (auto _ : state) {
        uint64_t sum = 0;
        for (size_t i = 0; i < N; i++) {
            sum += table[i].user.id;
        }
        benchmark::DoNotOptimize(sum);
    }
}

// --------------------------------------------------
// Benchmark 3:
// Allocation strategy
//
// Hypothesis:
// One contiguous allocation performs better
// than many small allocations.
// --------------------------------------------------

static void BM_ManySmallAllocations(benchmark::State& state) {
    size_t N = state.range(0);
    for (auto _ : state) {
        std::vector<conn_ptr_t*> table;
        table.reserve(N);
        for (size_t i = 0; i < N; i++) {
            table.push_back(new conn_ptr_t());
        }
        benchmark::ClobberMemory();
        for (auto* conn : table) {
            delete conn;
        }
    }
}

static void BM_OneContiguousAllocation(benchmark::State& state) {
    size_t N = state.range(0);
    for (auto _ : state) {
        std::vector<conn_embedded_t> table(N);
        benchmark::ClobberMemory();
    }
}

// -------------------------------------------
// Benchmark 4: Current AOS Approach
// -------------------------------------------

// Here we're testing out our production structs. Here we make some reasonable modifications, so 
// it's not the exact setup currently in production:
// a. Iterating over an array of structs rather than the pointers
// b. Removed the ip address struct that filled most of the conn_t struct.
// c. Removed user_t::password which wasn't used in the hot loop of our original implementation.
// c. Reordered some fields to ensure memory alignment and reduce padding.
// These are reasonable modifications based on what we know about cache locality and DoD.
// This is the hypothetical "best" version of our production structs, which should give 
// the SOA approach a challenge. Since we replaced our pointers, I'll make two logical modifications
// so that our tests runs reasonably:
// 1. If conn_t::fd == -1, then the connection is inactive.
// 2. If conn_t::user::id == 0 (since it's unsigned), then the connection is unauthenticated.
// Both of these are reasonable changes when migrating from pointers to "regular" fields.

struct user_prod_t {
  std::string username;
  uint32_t id = 0; 
};

typedef struct {
  std::vector<uint8_t> incoming;
  std::vector<uint8_t> outgoing;
  user_prod_t user;
  int fd = -1;
  bool want_read = false;
  bool want_write = false;
  bool want_close = false;
} conn_t;

/**
 * Creates a table of active and authenticated connections of size n
 */
static std::vector<conn_t> create_connection_table(int64_t n) {
    std::vector<conn_t> conn_table;
    conn_table.resize(n);
     for (size_t i = 0; i < n; i++) {
        conn_table[i].fd = i;
        conn_table[i].user.id = i;
        conn_table[i].user.username = "user" + std::to_string(i);
    }   
    return conn_table;
}

static void BM_AOS_World_Broadcast_AUTH(benchmark::State& state) {
    int64_t N{state.range(0)};
    std::vector<conn_t> conn_table = create_connection_table(N);
    int sender_conn_fd = conn_table[0].fd;
    std::vector<uint8_t> packet(128); 
    for (auto _ : state) {
        for (size_t i{0}; i < N; i++) {

            // Reject when not active or not authenticated, or when we're the sender
            // NOTE: Only a valid target when it's active AND authenticated
            conn_t& conn = conn_table[i];
            bool is_valid_target = (conn.fd != -1) & (conn.user.id != 0);
            bool is_sender = conn.fd == sender_conn_fd;
            if (!is_valid_target || is_sender) {
                continue;
            }

            // Copy simulated packet into outgoing buffer
            std::vector<uint8_t>& buffer = conn.outgoing;
            buffer.insert(
                buffer.end(),
                packet.begin(),
                packet.end()
            );
        }
    }
}

static void BM_AOS_World_Broadcast_MIXED(benchmark::State& state) {
    int64_t N{state.range(0)};
    std::vector<conn_t> conn_table = create_connection_table(N);
    std::mt19937 prng(42); // fixed seed for experiment replication.
    std::uniform_int_distribution<int> dist(0, 2);
    for (size_t i{0}; i < N; i++) {
        conn_t& conn = conn_table[i];
        int roll = dist(prng);
        if (roll == 0) {
            // Inactive and unauthenticated
            conn.fd = -1;
            conn.user.id = 0;
        } else if (roll == 1) {
            // Active but unauthenticated
            conn.user.id = 0;
        } else if (roll == 2) {
            // Active and authenticated
            continue;
        }   
    }

    int sender_conn_fd = conn_table[0].fd;
    std::vector<uint8_t> packet(128);
    for (auto _ : state) {
        for (size_t i{0}; i < N; i++) {
            // Reject when not active or not authenticated, or when we're the sender
            conn_t& conn = conn_table[i];
            bool is_valid_target = (conn.fd != -1) & (conn.user.id != 0);
            bool is_sender = conn.fd == sender_conn_fd;
            if (!is_valid_target || is_sender) {
                continue;
            }

            // Copy simulated packet into outgoing buffer
            std::vector<uint8_t>& buffer = conn.outgoing;
            buffer.insert(
                buffer.end(),
                packet.begin(),
                packet.end()
            );
        }
    }
}

static void BM_AOS_P2P_Broadcast_AUTH(benchmark::State& state) {
    int64_t N{state.range(0)};
    std::vector<conn_t> conn_table = create_connection_table(N);
    int sender_conn_fd = conn_table[0].fd;
    std::string& recipient_username = conn_table[N-1].user.username;
    std::vector<uint8_t> packet(128);
    for (auto _ : state) {
        for (size_t i{0}; i < N; i++) {
            // Reject when not active or not authenticated, or when we're the sender
            conn_t& conn = conn_table[i];
            bool is_valid_target = (conn.fd != -1) & (conn.user.id != 0);
            bool is_sender = conn.fd == sender_conn_fd;
            if (!is_valid_target || is_sender) {
                continue;
            }

            if (conn.user.username != recipient_username) {
                continue;
            }
            // Copy simulated packet into outgoing buffer
            std::vector<uint8_t>& buffer = conn.outgoing;
            buffer.insert(
                buffer.end(),
                packet.begin(),
                packet.end()
            );
        }
    }
}

static void BM_AOS_P2P_Broadcast_MIXED(benchmark::State& state) {
    int64_t N{state.range(0)};
    std::vector<conn_t> conn_table = create_connection_table(N);
    std::mt19937 prng(42); // fixed seed
    std::uniform_int_distribution<int> dist(0, 2); // 3 states
    for (size_t i{0}; i < N; i++) {
        conn_t& conn = conn_table[i];
        int roll = dist(prng);
        if (roll == 0) {
            // Inactive and unauthenticated
            conn.fd = -1;
            conn.user.id = 0;
        } else if (roll == 1) {
            // Active but unauthenticated
            conn.user.id = 0;
        } else if (roll == 2) {
            continue;
        }   
    }

    
    // Ensure last connection is considered valid
    conn_table[N-1].fd = N-1;
    conn_table[N-1].user.id = N-1;
    std::string& recipient_username = conn_table[N-1].user.username;
    std::vector<uint8_t> packet(128);
    int sender_conn_fd = conn_table[0].fd;
    for (auto _ : state) {
        for (size_t i{0}; i < N; i++) {
            // Reject when not active or not authenticated, or when we're the sender
            conn_t& conn = conn_table[i];
            bool is_valid_target = (conn.fd != -1) & (conn.user.id != 0);
            bool is_sender = conn.fd == sender_conn_fd;
            if (!is_valid_target || is_sender) {
                continue;
            }

            if (conn.user.username != recipient_username) {
                continue;
            }

            // Copy simulated packet into outgoing buffer
            std::vector<uint8_t>& buffer = conn.outgoing;
            buffer.insert(
                buffer.end(),
                packet.begin(),
                packet.end()
            );
        }
    }
}

// -------------------------------------------
// Benchmark 5: SOA Tests
// -------------------------------------------
enum class ConnFlags : uint8_t {
    NONE       = 0,
    WANT_READ  = 1 << 0,
    WANT_WRITE = 1 << 1,
    WANT_CLOSE = 1 << 2,
    IS_ACTIVE  = 1 << 3, 
    IS_AUTH    = 1 << 4, 
};

ConnFlags operator|(ConnFlags lhs, ConnFlags rhs) {
    return static_cast<ConnFlags>(static_cast<uint8_t>(lhs) | static_cast<uint8_t>(rhs));
}

ConnFlags operator&(ConnFlags lhs, ConnFlags rhs) {
    return static_cast<ConnFlags>(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
}

ConnFlags operator|=(ConnFlags lhs, ConnFlags rhs) {
    lhs = lhs | rhs;
    return lhs;
}

ConnFlags operator~(ConnFlags flag) {
    return static_cast<ConnFlags>(~static_cast<uint8_t>(flag));
}

ConnFlags operator&=(ConnFlags lhs, ConnFlags rhs) {
    lhs = static_cast<ConnFlags>(static_cast<uint8_t>(lhs) & static_cast<uint8_t>(rhs));
    return lhs;
}

// Helper for checking flags: returns true if the flag is set
bool has_flag(ConnFlags mask, ConnFlags flag) {
    return (static_cast<uint8_t>(mask) & static_cast<uint8_t>(flag)) != 0;
}

struct ConnectionManager {
    std::vector<int> fds;
    std::vector<ConnFlags> flags;
    std::vector<std::vector<uint8_t>> incoming_buffers;
    std::vector<std::vector<uint8_t>> outgoing_buffers;        
    std::vector<int> user_ids;
    std::vector<std::string> usernames;
};

static ConnectionManager create_connection_manager(int64_t N) {
    ConnectionManager manager;
    manager.fds.reserve(N);
    manager.flags.reserve(N);
    manager.incoming_buffers.reserve(N);
    manager.outgoing_buffers.reserve(N);
    manager.user_ids.reserve(N);
    manager.usernames.reserve(N);
    for (size_t i = 0; i < N; i++) {
        manager.fds.push_back(i);
        manager.flags.push_back(ConnFlags::NONE); // Example flag value
        manager.incoming_buffers.emplace_back(); // Empty buffer
        manager.outgoing_buffers.emplace_back(); // Empty buffer
        manager.user_ids.push_back(i); // Example user ID
        manager.usernames.push_back("user" + std::to_string(i)); // Example username
    }   
    return manager;
}

/**
 * World Broadcast with all connections as active and authenticate, meaning 
 * all connections will be recipients of the packet. 
 * Hypothesis: Since everyone will receive the message, this test has minimum
 * branch mispredictions, so we assume that this will be more performant 
 * than the test that mixes the flags.
 */
static void BM_SOA_World_Broadcast_AUTH(benchmark::State& state) {
    int64_t N{state.range(0)};
    ConnectionManager manager = create_connection_manager(N);    

    // 1. Set flags for the connection to be active and authenticated
    for (auto& conn : manager.flags) {
        conn = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH;
    }

    // 2. Iterate through connections
    int sender_conn_fd = manager.fds[0];
    std::vector<uint8_t> packet(128); // A message of 128 bytes that we'll append to every outgoing buffer
    const ConnFlags REQUIRED = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH; // Mask combination required for receiving data
    for (auto _ : state) {
        for (size_t i{0}; i < N; i++) {
            // Reject when not active or not authenticated, or when we're the sender
            bool is_valid_target = (manager.flags[i] & REQUIRED) == REQUIRED;
            bool is_sender = (sender_conn_fd == manager.fds[i]);
            if (!is_valid_target || is_sender) {
                continue;
            }

            // Copy simulated packet into outgoing buffer
            std::vector<uint8_t>& buffer = manager.outgoing_buffers[i];
            buffer.insert(
                buffer.end(),
                packet.begin(),
                packet.end()
            );
        }
    }
}

/**
 * World broadcast where there's a mix of authenticated, unauthenticated, and in-active connections.
 * Not all connections will receive the target data, and the status of each should be randomly
 * distributed. This test should maximize branch mispredictions, and have worse performance than the 
 * benchmark with everyone has active and authenticated.
 */
static void BM_SOA_World_Broadcast_MIXED(benchmark::State& state) {
    int64_t N{state.range(0)};
    auto manager{create_connection_manager(N)};

    std::mt19937 prng(42); // fixed seed
    std::uniform_int_distribution<int> dist(0, 2); // 3 states
    for (size_t i{0}; i < N; i++) {
        ConnFlags& current_flags = manager.flags[i];
        int roll = dist(prng);
        if (roll == 0) {
            // Inactive and unauthenticated
            continue;
        } else if (roll == 1) {
            // Active but unauthenticated
            current_flags = ConnFlags::IS_ACTIVE;
        } else if (roll == 2) {
            // Authenticated, which implies it is active as well.
            current_flags = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH;
        }   
    }

    // 2. Iterate through the connections
    int sender_conn_fd = manager.fds[0];
    std::vector<uint8_t> packet(128);
    const ConnFlags REQUIRED = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH;
    for (auto _ : state) {
        for (size_t i{0}; i < N; i++) {
            // Reject when not active or not authenticated, or when we're the sender
            bool is_valid_target = (manager.flags[i] & REQUIRED) == REQUIRED;
            bool is_sender = (sender_conn_fd == manager.fds[i]);
            if (!is_valid_target || is_sender) {
                continue;
            }
            
            std::vector<uint8_t>& buffer = manager.outgoing_buffers[i];
            buffer.insert(
                buffer.end(),
                packet.begin(),
                packet.end()
            );
        }
    }
}

/**
 * Simulates a P2P broadcast where all other connections are a potential candidate for being 
 * the recipient candidate that we want to send our data to. Here we assume the target user is the last 
 * user in the connection table, allowing us to estimate the real p99/worse case latency 
 * 
 * @note P2P broadcast involve std::string username comparisons, and this makes 
 * them warrant being their own tests. This case where all users are authenticated and active
 * should complete faster than the next benchmark which has connections of mixed/random statuses.
 */
static void BM_SOA_P2P_Broadcast_AUTH(benchmark::State& state) {
    int64_t N{state.range(0)};
    auto manager{create_connection_manager(N)};

    // Set flags for the connection to be active and authenticated
    for (auto& conn : manager.flags) {
        conn = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH;
    }

    int sender_conn_fd = manager.fds[0];
    std::string& target_username = manager.usernames[N-1];
    std::vector<uint8_t> packet(128);
    const ConnFlags REQUIRED = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH;
    for (auto _ : state) {
        for (size_t i{0}; i < N; i++) {
            bool is_valid_target = (manager.flags[i] & REQUIRED) == REQUIRED;
            bool is_sender = (sender_conn_fd == manager.fds[i]);
            if (!is_valid_target || is_sender) {
                continue;
            }

            // Reject if different username
            std::string& current_username = manager.usernames[i];
            if (current_username != target_username) {
                continue;
            }
            std::vector<uint8_t>& buffer = manager.outgoing_buffers[i];
            buffer.insert(
                buffer.end(),
                packet.begin(),
                packet.end()
            );
        }
    }
}

/**
 * Simulates a P2P broadcast where there's a mix of authenticated, inactive, and unauthenticated connections.
 * Again assume the worst case and the target user is the last user in the array.
 */
static void BM_SOA_P2P_Broadcast_MIXED(benchmark::State& state) {
    int64_t N{state.range(0)};
    auto manager{create_connection_manager(N)};
    std::mt19937 prng(42);
    std::uniform_int_distribution<int> dist(0, 2); // 3 states
    for (size_t i{0}; i < N; i++) {
        ConnFlags& current_flags = manager.flags[i];
        int roll = dist(prng);
        if (roll == 0) {
            // Inactive and unauthenticated
            continue;
        } else if (roll == 1) {
            // Active but unauthenticated
            current_flags = ConnFlags::IS_ACTIVE;
        } else if (roll == 2) {
            // Authenticated, which implies it is active as well.
            current_flags = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH;
        }
    }


    // Ensures the last connection is valid so that it can be hit.
    int sender_conn_fd = manager.fds[0];
    manager.flags[N-1] = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH;
    std::string& target_username = manager.usernames[N-1];
    std::vector<uint8_t> packet(128);
    const ConnFlags REQUIRED = ConnFlags::IS_ACTIVE | ConnFlags::IS_AUTH;
     for (auto _ : state) {
        for (size_t i{0}; i < N; i++) {
            bool is_valid_target = (manager.flags[i] & REQUIRED) == REQUIRED;
            bool is_sender = (sender_conn_fd == manager.fds[i]);
            if (!is_valid_target || is_sender) {
                continue;
            }
            if (target_username != manager.usernames[i]) {
                continue;
            }
            std::vector<uint8_t>& buffer = manager.outgoing_buffers[i];
            buffer.insert(
                buffer.end(),
                packet.begin(),
                packet.end()
            );
        }
    }
}

// --------------------------------------------------
// Test Registration
// --------------------------------------------------

BENCHMARK(BM_Traversal_Indirect)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_Traversal_Direct)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_UserAccess_Indirect)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_UserAccess_Direct)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_ManySmallAllocations)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_OneContiguousAllocation)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_AOS_World_Broadcast_AUTH)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_AOS_World_Broadcast_MIXED)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_AOS_P2P_Broadcast_AUTH)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);
BENCHMARK(BM_AOS_P2P_Broadcast_MIXED)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_SOA_World_Broadcast_AUTH)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_SOA_World_Broadcast_MIXED)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_SOA_P2P_Broadcast_AUTH)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK(BM_SOA_P2P_Broadcast_MIXED)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000);

BENCHMARK_MAIN();