#include <catch2/catch_test_macros.hpp>
#include "ring_buffer.hpp"

TEST_CASE("pop from empty returns nullopt", "[ring_buffer]")
{
    RingBuffer<int, 4> rb;
    REQUIRE(!rb.pop().has_value());
    REQUIRE(rb.empty());
}

TEST_CASE("push and pop round-trip", "[ring_buffer]")
{
    RingBuffer<int, 4> rb;
    REQUIRE(rb.push(42));
    REQUIRE(!rb.empty());
    auto v = rb.pop();
    REQUIRE(v.has_value());
    REQUIRE(*v == 42);
    REQUIRE(rb.empty());
}

TEST_CASE("fills to N-1 slots then rejects", "[ring_buffer]")
{
    RingBuffer<int, 4> rb; // usable capacity: 3
    REQUIRE(rb.push(1));
    REQUIRE(rb.push(2));
    REQUIRE(rb.push(3));
    REQUIRE(!rb.push(4));
}

TEST_CASE("FIFO order is preserved", "[ring_buffer]")
{
    RingBuffer<int, 4> rb;
    rb.push(10);
    rb.push(20);
    rb.push(30);
    REQUIRE(*rb.pop() == 10);
    REQUIRE(*rb.pop() == 20);
    REQUIRE(*rb.pop() == 30);
}

TEST_CASE("wraparound preserves FIFO order", "[ring_buffer]")
{
    RingBuffer<int, 4> rb;
    rb.push(1);
    rb.push(2);
    rb.push(3);
    rb.pop();
    rb.pop(); // drain two, head and tail wrap
    rb.push(4);
    rb.push(5);
    REQUIRE(*rb.pop() == 3);
    REQUIRE(*rb.pop() == 4);
    REQUIRE(*rb.pop() == 5);
    REQUIRE(rb.empty());
}
