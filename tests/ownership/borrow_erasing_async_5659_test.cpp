// Spec-derived regression test for issue #5659.
//
// §9.7 / §30.1 (proposal 0146, v0.330): an async method may not have
// an IReadOnlySpan<TElement> parameter, because IReadOnlySpan<TElement>
// is read as holding borrows under every rule keyed on a value that
// holds borrows.
//
// This is the async-specific part of the four banked conformance cases:
//   s9_7_0030: async method Scan(s: IReadOnlySpan<int>)

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// True when an ERROR diagnostic whose message contains `sub` was emitted.
bool hasError(const OwnershipResult& r, const std::string& sub) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(sub) != std::string::npos) return true;
    }
    return false;
}

// The distinctive substring of the async borrow-bearing diagnostic.
const char* kParamMsg = "may not be a parameter of an `async`";
const char* kBorrowBearingMsg = "borrow-bearing";

}  // namespace

// s9_7_0030: an IReadOnlySpan parameter is read as holding borrows
// and may not be an async parameter
BEZEL_TEST(borrow_erasing_async_5659,
           i_read_only_span_async_param_rejected) {
    auto r = parseAndCheckOwnership(
        "module M; "
        "public class Client "
        "  public async method Scan(s: IReadOnlySpan<int>): Task "
        "  end Scan; "
        "end Client;");
    BEZEL_EXPECT_TRUE(hasError(r, kParamMsg));
    BEZEL_EXPECT_TRUE(hasError(r, kBorrowBearingMsg));
}