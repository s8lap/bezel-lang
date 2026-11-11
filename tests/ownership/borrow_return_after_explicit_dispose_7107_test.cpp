// Regression tests for #7107 -- an explicit `Dispose()` SUPPRESSED §28.3.8's
// borrow-lifetime rejection at a `return`: a borrow rooted at the disposed
// local (a bound method reference, a member read, a borrow-returning call)
// was returned to the caller, and the caller's use read a released block.
//
//   var d := new D();
//   d.Dispose();
//   return d.Dispose;      // compiled, with the §28.2 warning alone
//
// The twin WITHOUT the `d.Dispose();` line was already rejected ("borrow
// returned from `d` would outlive its source"), so one rule was enforced or
// not depending on whether the disposal was explicit.
//
// Language spec §28.3.8 (v0.357) "Borrow lifetime rules", the closure bullet:
//   "While its source set is non-empty, a function value may not be stored
//    in a field, ... may not be returned from the enclosing method (by
//    `return`, or by `yield return` in an iterator ...)"
// and its provenance clauses:
//   "1. ... a captured owned local [contributes] its enclosing scope ...
//    2. A **method reference** contributes its bound receiver's source, as a
//       capture."
//   "`obj.Method` is a function value whose source set is `obj`'s ... subject
//    to the closure-escape rules above as if `obj` had been captured by an
//    explicit lambda."
//
// Language spec §28.2 (v0.357) "The compiler-inserted free":
//   "An explicit `Dispose()` ... performs **logical disposal only** ... and
//    does not reclaim its receiver: the receiver's block stays live until the
//    holder's release point".
// So an explicitly disposed local is still scope-bound: its block is released
// at the local's scope end, exactly where a still-owned local's is, and a
// borrow of it may no more leave the method.
//
// Language spec §27.6 (v0.357): "instance methods (the instance is captured
// at formation -- by reference for class- or interface-typed receivers ...)".
//
// Root cause: `MethodOwnership::checkBorrowEscapesOnReturn` asked whether the
// root local was `Owned`; an explicit disposal leaves it `Disposed`, so the
// rule was skipped.
//
// The rows are COMPILE-TIME (`parseAndCheckOwnership`): the defect is a
// compile-time over-acceptance. Every row was measured on the base compiler
// compiling and then faulting under the generated-code ASan leg
// (heap-use-after-free or SEGV), by the lane's matrix (see the commit body).
// The first row is the issue's own repro, byte for byte.

#include <string>

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;

namespace {

// The §28.3.8 rejection, as the never-disposed twin words it ...
constexpr const char* kEscape =
    "borrow returned from `d` would outlive its source";
// ... and what this fix says about the source.
constexpr const char* kDisposedSource =
    "owned local `d` was explicitly disposed and its block is released at "
    "scope end";

bool hasError(const OwnershipResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool rejectedAsEscape(const OwnershipResult& r) {
    return hasError(r, kEscape) && hasError(r, kDisposedSource);
}

// `D` owns an `Inner`, lends it (`In`), lends itself (`Self`), and has an
// ordinary instance method to reference (`Ping`).
const std::string kHead =
    "module M;\n"
    "public class Inner\n"
    "  private _v: int;\n"
    "  public constructor(v: int)\n"
    "    _v := v;\n"
    "  end constructor;\n"
    "  public method V(): int => _v;\n"
    "end Inner;\n"
    "public class D\n"
    "  private owned _in: Inner;\n"
    "  public constructor(n: int)\n"
    "    _in := new Inner(n);\n"
    "  end constructor;\n"
    "  public property In: Inner => _in;\n"
    "  public method Self(): D => this;\n"
    "  public method Ping(): int => _in.V();\n"
    "end D;\n"
    "public class C\n";

const std::string kTail =
    "  public static method Main(): int\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

std::string prog(const std::string& members) {
    return kHead + members + kTail;
}

}  // namespace

// The issue's own repro, byte for byte: a method group over the disposed
// local is returned. (§27.6 v0.357 also forbids a reference to `Dispose`
// itself at its formation -- #7344; the escape is rejected whichever method
// the reference names, as the next row shows.)
BEZEL_TEST(ownership_borrow_return_after_explicit_dispose_7107,
           issue_repro_method_group_over_disposed_local_rejected_spec_28_3_8) {
    OwnershipResult r = parseAndCheckOwnership(
        "module M;\n"
        "public class D\n"
        "  public constructor()\n"
        "  end constructor;\n"
        "end D;\n"
        "public class C\n"
        "  public static method Make(): () -> ()\n"
        "    var d := new D();\n"
        "    d.Dispose();\n"
        "    return d.Dispose;\n"
        "  end Make;\n"
        "  public static method Main(): int\n"
        "    var f := C.Make();\n"
        "    f();\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(rejectedAsEscape(r));
}

// The same escape through a reference to an ORDINARY method (§27.6: the
// instance is captured by reference; §28.3.8 provenance clause 2).
BEZEL_TEST(ownership_borrow_return_after_explicit_dispose_7107,
           ordinary_method_reference_after_dispose_rejected_spec_28_3_8) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Make(): () -> int\n"
        "    var d := new D(7);\n"
        "    d.Dispose();\n"
        "    return d.Ping;\n"
        "  end Make;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsEscape(r));
}

// The kill spelled as a DISCARD (§28.2: "or a discard of the binding").
BEZEL_TEST(ownership_borrow_return_after_explicit_dispose_7107,
           method_reference_after_discard_rejected_spec_28_3_8) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Make(): () -> int\n"
        "    var d := new D(7);\n"
        "    _ := d;\n"
        "    return d.Ping;\n"
        "  end Make;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsEscape(r));
}

// The kill on BOTH arms of an `if`: it dominates the return on every path.
BEZEL_TEST(ownership_borrow_return_after_explicit_dispose_7107,
           method_reference_after_dispose_on_both_arms_rejected_spec_28_3_8) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Make(kill: bool): () -> int\n"
        "    var d := new D(7);\n"
        "    if kill then\n"
        "      d.Dispose();\n"
        "    else\n"
        "      d.Dispose();\n"
        "    end;\n"
        "    return d.Ping;\n"
        "  end Make;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsEscape(r));
}

// The rule is the RETURN's, not the method reference's: a member read rooted
// at the disposed local escapes the same way (§28.3.8, the return bullet).
BEZEL_TEST(ownership_borrow_return_after_explicit_dispose_7107,
           member_read_after_dispose_rejected_spec_28_3_8) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public method Get(): Inner\n"
        "    var d := new D(7);\n"
        "    d.Dispose();\n"
        "    return d.In;\n"
        "  end Get;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsEscape(r));
}

// ... and after the null-conditional kill `d?.Dispose()`.
BEZEL_TEST(ownership_borrow_return_after_explicit_dispose_7107,
           member_read_after_null_conditional_dispose_rejected_spec_28_3_8) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public method Get(): Inner\n"
        "    var d := new D(7);\n"
        "    d?.Dispose();\n"
        "    return d.In;\n"
        "  end Get;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsEscape(r));
}

// A borrow-returning CALL on the disposed local (`Self(): D => this`).
BEZEL_TEST(ownership_borrow_return_after_explicit_dispose_7107,
           borrow_returning_call_after_dispose_rejected_spec_28_3_8) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public method Get(): D\n"
        "    var d := new D(7);\n"
        "    d.Dispose();\n"
        "    return d.Self();\n"
        "  end Get;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsEscape(r));
}

// An EARLIER read of the disposed local (which draws the §28.2 diagnostic of
// its own) does not hide the escape at the later return.
BEZEL_TEST(ownership_borrow_return_after_explicit_dispose_7107,
           method_reference_after_dispose_and_earlier_read_rejected_spec_28_3_8) {
    OwnershipResult r = parseAndCheckOwnership(prog(
        "  public static method Make(): () -> int\n"
        "    var d := new D(7);\n"
        "    d.Dispose();\n"
        "    var k := d.Ping();\n"
        "    return d.Ping;\n"
        "  end Make;\n"));
    BEZEL_EXPECT_TRUE(rejectedAsEscape(r));
}
