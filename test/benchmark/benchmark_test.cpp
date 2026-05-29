#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>
#include <random>
#include <algorithm>

// --------------------------------------------------
// Test Types
// --------------------------------------------------

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

// --------------------------------------------------
// Helpers
// --------------------------------------------------

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

static void BM_UserAccess_Embedded(benchmark::State& state) {
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

// ---------------------------
// In-memory username lookup, 
// ---------------------------




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

BENCHMARK(BM_UserAccess_Embedded)
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

BENCHMARK_MAIN();