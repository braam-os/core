#include "harness.h"

#include "kernel/result.h"

namespace {
Result<u32> halve(u32 n) {
    if (n % 2)
        return Err(Error::Invalid);
    return n / 2;
}

// TRY propagates the error out of the enclosing function unchanged.
Result<u32> quarter(u32 n) {
    u32 half = TRY(halve(n));
    return TRY(halve(half));
}

Result<void> require_even(u32 n) {
    if (n % 2)
        return Err(Error::Invalid);
    return {};
}

Result<u32> checked(u32 n) {
    TRY_VOID(require_even(n));
    return n;
}
} // namespace

void test_result() {
    test_begin("result");

    Option<u32> none;
    CHECK(!none.has_value());
    CHECK(!none);
    CHECK_EQ(none.value_or(7), 7);

    Option<u32> some = 42;
    CHECK(some.has_value());
    CHECK_EQ(some.value(), 42);
    CHECK_EQ(some.value_or(7), 42);

    some = None;
    CHECK(!some);

    Result<u32> ok = halve(10);
    CHECK(ok.is_ok());
    CHECK(!ok.is_err());
    CHECK_EQ(ok.value(), 5);

    Result<u32> bad = halve(7);
    CHECK(bad.is_err());
    CHECK(bad.error() == Error::Invalid);
    CHECK_EQ(bad.value_or(99), 99);

    CHECK_EQ(quarter(20).value(), 5);
    CHECK(quarter(10).is_err()); // 5 is odd, so the second halve fails
    CHECK(quarter(7).is_err());  // the first halve fails

    CHECK(require_even(4).is_ok());
    CHECK(require_even(5).is_err());
    CHECK_EQ(checked(8).value(), 8);
    CHECK(checked(9).is_err());

    CHECK(error_name(Error::NotFound) == "not found");
}
