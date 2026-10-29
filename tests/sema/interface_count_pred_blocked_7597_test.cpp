// fix #7597 (§26.5 rule 1, v0.350; stdlib §1 "Block-by-name", §4.1): the
// built-in collection INTERFACES inherit the instance property `Count`
// (`IReadOnlyCollection<T>`, stdlib §4.1), so §26.5 rule 1 commits instance
// resolution by name and the §5 predicate `Count(pred)` is unreachable on an
// `IList<T>` / `IReadOnlyList<T>` / `ICollection<T>` / `IReadOnlyCollection<T>`
// receiver, exactly as on `Array<T>` (#3342): "the call is a compile error
// citing the instance member(s)". Before the fix `l.Count(pred)` compiled and
// ran. The zero-arg `l.Count()`, the property read `l.Count`, and receivers
// with no instance `Count` (`IEnumerable<T>`, the composed
// `l.Where(pred).Count()`) stay accepted.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool hasBlockError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("`Count`") != std::string::npos &&
            d.message.find("instance member") != std::string::npos &&
            d.message.find("26.5") != std::string::npos &&
            d.message.find("rule 1") != std::string::npos)
            return true;
    }
    return false;
}

bezel::test::SemaResult onRecv(const std::string& ty, const std::string& use) {
    return parseSemaAll(
        "module M; import System; import System.Linq; "
        "class C public static method Main(): int "
        "  var a: Array<int> := [1, 2, 3]; "
        "  var c: " + ty + " := a; "
        "  return " + use + "; "
        "end Main; end C;");
}

}  // namespace

#define COUNT_PRED "c.Count(lambda(x) => x > 1)"

BEZEL_TEST(sema_interface_count_pred_blocked_7597, ilist_rejected) {
    auto r = onRecv("IList<int>", COUNT_PRED);
    BEZEL_EXPECT_TRUE(hasBlockError(r));
}
BEZEL_TEST(sema_interface_count_pred_blocked_7597, ireadonlylist_rejected) {
    auto r = onRecv("IReadOnlyList<int>", COUNT_PRED);
    BEZEL_EXPECT_TRUE(hasBlockError(r));
}
BEZEL_TEST(sema_interface_count_pred_blocked_7597, icollection_rejected) {
    auto r = onRecv("ICollection<int>", COUNT_PRED);
    BEZEL_EXPECT_TRUE(hasBlockError(r));
}
BEZEL_TEST(sema_interface_count_pred_blocked_7597,
           ireadonlycollection_rejected) {
    auto r = onRecv("IReadOnlyCollection<int>", COUNT_PRED);
    BEZEL_EXPECT_TRUE(hasBlockError(r));
}
// Controls: still accepted.
BEZEL_TEST(sema_interface_count_pred_blocked_7597, zero_arg_call_accepted) {
    auto r = onRecv("IList<int>", "c.Count()");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_interface_count_pred_blocked_7597, property_read_accepted) {
    auto r = onRecv("IList<int>", "c.Count");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_interface_count_pred_blocked_7597,
           ienumerable_count_pred_accepted) {
    auto r = onRecv("IEnumerable<int>", COUNT_PRED);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_interface_count_pred_blocked_7597,
           composed_where_count_accepted) {
    auto r = onRecv("IList<int>", "c.Where(lambda(x) => x > 1).Count()");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
BEZEL_TEST(sema_interface_count_pred_blocked_7597,
           user_class_own_count_pred_accepted) {
    auto r = parseSemaAll(
        "module M; import System; import System.Linq; "
        "class Bag public method Count(p: (int) -> bool): int => 7; end Bag; "
        "class C public static method Main(): int "
        "  var b := new Bag(); "
        "  return b.Count(lambda(x) => x > 1); "
        "end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
