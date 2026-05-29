// Spec-derived tests for Ownership slice O13 — method calls returning
// an `owned T` value (§22.3, §28.3).
//
// A method declared `method M(): owned T` returns ownership of the
// produced value; the caller incurs the same discharge obligation as
// for `new T()`. A method declared `method M(): T` (without owned)
// returns a borrowed reference and carries no obligation.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

std::string mod(const std::string& body) {
    return std::string("module M; ") + body;
}

} // namespace

BEZEL_TEST(own_return, owned_return_into_plain_var_undischarged_diagnoses) {
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class C "
        "  public method Make(): owned T => new T(); "
        "  public method M() "
        "    var x := this.Make(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_return, owned_return_into_using_var_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class C "
        "  public method Make(): owned T => new T(); "
        "  public method M() "
        "    using var x := this.Make(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_return, borrowed_return_into_plain_var_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class C "
        "  public method Get(): T => new T(); "
        "  public method M() "
        "    var x := this.Get(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_return, owned_return_dispatched_on_param) {
    auto r = parseAndCheckOwnership(mod(
        "class T end T; "
        "class Source public method Read(): owned T => new T(); end Source; "
        "class C "
        "  public method M(src: Source) "
        "    var x := src.Read(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_return, owned_return_explicit_dispose_clean) {
    auto r = parseAndCheckOwnership(mod(
        "class T public method Dispose() _ := 1; end Dispose; end T; "
        "class C "
        "  public method Make(): owned T => new T(); "
        "  public method M() "
        "    var x := this.Make(); "
        "    x.Dispose(); "
        "  end M; "
        "end C;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
