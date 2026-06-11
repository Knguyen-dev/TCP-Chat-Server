#include <cassert>
#include <iostream>
#include "shared.hpp"


void test_OR() {
    ConnFlags flags{ConnFlags::WANT_READ | ConnFlags::WANT_WRITE};

    // Verify both bits are set
    assert(has_flag(flags, ConnFlags::WANT_READ));
    assert(has_flag(flags, ConnFlags::WANT_WRITE));

    // Verify that other flags aren't set
    assert(!has_flag(flags, ConnFlags::IS_AUTH));
    assert(!has_flag(flags, ConnFlags::WANT_CLOSE));
    assert(!has_flag(flags, ConnFlags::NONE));
    std::cout << "[PASS] test_OR\n";
}

void test_AND() {
    ConnFlags mask{ConnFlags::WANT_READ | ConnFlags::IS_ACTIVE};

    // Bitwise AND should isolate the flag if it exists
    // Isolate want read and is active as they should exist
    ConnFlags result = mask & ConnFlags::WANT_READ;
    assert(result == ConnFlags::WANT_READ);
    result = mask & ConnFlags::IS_ACTIVE;
    assert(result == ConnFlags::IS_ACTIVE);

    // Flags that aren't set shouldn't exist
    result = mask & ConnFlags::WANT_WRITE;
    assert(result == ConnFlags::NONE);
    std::cout << "[PASS] test_AND\n";
}

void test_negate() {
    ConnFlags read_flag{ConnFlags::WANT_READ};
    ConnFlags inverted = ~read_flag;

    // Want read should be 0, whilst all other flags are set 1
    assert(!has_flag(inverted, ConnFlags::WANT_READ));
    assert(has_flag(inverted, ConnFlags::WANT_CLOSE));
    assert(has_flag(inverted, ConnFlags::WANT_WRITE));
    assert(has_flag(inverted, ConnFlags::IS_ACTIVE));
    assert(has_flag(inverted, ConnFlags::IS_AUTH));
    std::cout << "[PASS] test_negate\n";
}

void test_assignment_OR() {
    ConnFlags flags{ConnFlags::IS_ACTIVE};
    flags |= ConnFlags::IS_AUTH;

    // Active and auth should be set 1, all others should be 0
    assert(has_flag(flags, ConnFlags::IS_ACTIVE));
    assert(has_flag(flags, ConnFlags::IS_AUTH));
    assert(!has_flag(flags, ConnFlags::WANT_READ));
    assert(!has_flag(flags, ConnFlags::WANT_WRITE));
    assert(!has_flag(flags, ConnFlags::WANT_CLOSE));

    std::cout << "[PASS] test_assignment_OR\n";
}

void test_assignment_AND() {
    ConnFlags flags{ConnFlags::WANT_READ | ConnFlags::WANT_WRITE};

    // Unset the WANT_READ flag using AND + negation
    flags &= ~ConnFlags::WANT_READ;

    // Expect the WANT_READ to be 0, WANT_WRITE to be 1
    assert(!has_flag(flags, ConnFlags::WANT_READ));
    assert(has_flag(flags, ConnFlags::WANT_WRITE));

    // Everything else unchanged, still zero
    assert(!has_flag(flags, ConnFlags::IS_ACTIVE));
    assert(!has_flag(flags, ConnFlags::IS_AUTH));
    assert(!has_flag(flags, ConnFlags::WANT_CLOSE));

    std::cout << "[PASS] test_assignment_AND\n";
}

int main() {
    std::cout << "Running ConnFlags Unit Tests...\n";
    test_OR();
    test_AND();
    test_negate();
    test_assignment_OR();
    test_assignment_AND();

    std::cout << "All tests passed successfully!\n";
    return 0;
}