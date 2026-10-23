// Codegen: a collection-literal temporary given to a PLAIN (non-`owned`)
// constructor parameter that the callee stores is disposed at its boundary, and
// a read through the stored borrow AFTER that point raises
// DisposedAccessException in checked disposal mode.
//
// Issue #7045 / §28.3 (v0.340) *Borrow lifetime rules*, clause 3, and §28.2
// (v0.340) *Use after disposal; checked disposal mode*; §22.10 (v0.340)
// (the primary constructor's synthesized capture field is the same
// plain-parameter store as an explicit `_xs := xs`).
//
// THE RULE. §28.3 clause 3: "an unbound owned temporary or a borrow of a
// scope-bound local, passed to a plain parameter that the callee stores ... is
// admitted at the call ... **Neither source is lifetime-extended**: the
// temporary is disposed at its K8 boundary and the local at its scope's end,
// and a member access through the stored borrow after that point is the
// backstop's fault." The backstop is §28.2 K4.1: in CHECKED disposal mode a
// member access on a logically disposed instance raises
// DisposedAccessException (§31.4). The owner's ruling for #7045 spells out
// the shape this file pins: "`for var x in _xs` enumerates the array, so it is
// such an access."
//
// BEFORE THE FIX the release of the temporary RECLAIMED the bz_array_t header
// (the scope-end `it->collection` arm called bz_array_free unconditionally,
// in checked mode as well as unchecked), so the stored borrow aliased freed
// memory: MEASURED under generated-code ASan, `heap-use-after-free ... READ of
// size 4 ... in bz$Conf.B$Sum` for the explicit-constructor program and
// `in bz$Conf.A$Sum` for the primary-constructor one — in BOTH the default and
// the checked-disposal builds. Without ASan the same programs printed garbage
// (MEASURED: -920210659 and -1711794406) instead of the expected value.
//
// THE FIX. Two halves, both in codegen:
//   (1) the scope-end release of a collection QUARANTINES in checked
//       disposal mode (bz_array_mark_disposed / bz_dict_mark_disposed)
//       instead of reclaiming, so the header's disposed byte stays readable
//       for the backstop's flag test — §28.2's latitude: "the implementation
//       may defer reclaiming a disposed instance's memory in checked mode
//       (quarantine, delayed free) so that the flag remains readable after
//       disposal". This is the same discipline the class-instance path
//       (emitReclaimBlock) and the explicit `.Dispose()` call site
//       (bz_array_mark_disposed) already use;
//   (2) the `for var x in <array>` enumeration emits the disposed-flag guard
//       (bz_array_is_disposed + emitGuardThrow) that the Count / indexer /
//       collection-method guards already emit — the enumeration was the one
//       array read path with NO guard.
//
// Test groups:
//   1-2. The two corpus programs: a literal to a plain explicit-constructor
//        parameter, and a literal to a plain primary-constructor parameter.
//        Each reads the stored field AFTER the temporary's boundary →
//        DisposedAccessException, caught and printed as "faulted".
//   3.   The named-local twin: a scope-bound local passed to the same plain
//        parameter and read after the local's scope ends → "faulted" too (it
//        is the same store, with the local as the borrow source).
//   4-5. The remedies the rule names, which must PRINT the values: the
//        parameter declared `owned` (the callee owns the collection and
//        disposes it with the object, §22.10 / §28.8), and the literal bound
//        to a `using var` declared BEFORE the holder (so the source outlives
//        the read). Both print 6 in the default AND the checked build.
//   6.   The `scoped` remedy does NOT apply to a collection-typed parameter:
//        §28.14 admits `scoped` only on a function-typed, class-typed, or
//        interface-typed parameter, so the declaration is rejected — the
//        compiler must not silently accept the shape.
//
// The ASan/LSan leg is run by the harness with leak detection OFF for the
// checked-disposal rows, because a quarantined block is RETAINED BY DESIGN
// (§28.2's quarantine — the same property the #4602 bounded-retention test
// documents: "leaks not checked (an escaped block is quarantined by design)").
// The default-build rows (4-6) run with leak detection ON.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

// The two corpus programs (tests/conformance/rules/cases/s28_3_0205.bzl and
// s28_3_0206.bzl), verbatim in shape.
const char* kExplicitCtor =
    "module Conf;\n"
    "public class B\n"
    "  private _xs: Array<int>;\n"
    "  public constructor(xs: Array<int>)\n"
    "    _xs := xs;\n"
    "  end;\n"
    "  public method Sum(): int\n"
    "    var s := 0;\n"
    "    for var x in _xs do s := s + x; end;\n"
    "    return s;\n"
    "  end Sum;\n"
    "end B;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var h: B? := null;\n"
    "    do\n"
    "      h := new B([1, 2, 3]);\n"
    "    end;\n"
    "    try\n"
    "      Console.WriteLine(h!.Sum());\n"
    "    catch\n"
    "      DisposedAccessException => Console.WriteLine(\"faulted\"),\n"
    "    end try;\n"
    "  end Main;\n"
    "end Program;\n";

const char* kPrimaryCtor =
    "module Conf;\n"
    "public class A(xs: Array<int>)\n"
    "  public method Sum(): int\n"
    "    var s := 0;\n"
    "    for var x in xs do s := s + x; end;\n"
    "    return s;\n"
    "  end Sum;\n"
    "end A;\n"
    "public class H\n"
    "  private owned _a: A;\n"
    "  public constructor()\n"
    "    _a := new A([4, 5, 6]);\n"
    "  end;\n"
    "  public method Go(): int => _a.Sum();\n"
    "end H;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var h := new H();\n"
    "    try\n"
    "      Console.WriteLine(h.Go());\n"
    "    catch\n"
    "      DisposedAccessException => Console.WriteLine(\"faulted\"),\n"
    "    end try;\n"
    "  end Main;\n"
    "end Program;\n";

// The named-local twin of the explicit-constructor program: the borrow source
// is a scope-bound local rather than a temporary, and the local is disposed at
// the end of its `do` block.
const char* kNamedLocalTwin =
    "module Conf;\n"
    "public class B\n"
    "  private _xs: Array<int>;\n"
    "  public constructor(xs: Array<int>)\n"
    "    _xs := xs;\n"
    "  end;\n"
    "  public method Sum(): int\n"
    "    var s := 0;\n"
    "    for var x in _xs do s := s + x; end;\n"
    "    return s;\n"
    "  end Sum;\n"
    "end B;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var h: B? := null;\n"
    "    do\n"
    "      var xs: Array<int> := [1, 2, 3];\n"
    "      h := new B(xs);\n"
    "    end;\n"
    "    try\n"
    "      Console.WriteLine(h!.Sum());\n"
    "    catch\n"
    "      DisposedAccessException => Console.WriteLine(\"faulted\"),\n"
    "    end try;\n"
    "  end Main;\n"
    "end Program;\n";

// Remedy 1: the parameter is declared `owned`, so the callee owns the
// collection and disposes it with the object (§22.10 / §28.8) — the stored
// field is not a borrow, so the read is defined and prints the value.
const char* kOwnedParam =
    "module Conf;\n"
    "public class B\n"
    "  private owned _xs: Array<int>;\n"
    "public constructor(owned xs: Array<int>)\n"
    "    _xs := xs;\n"
    "  end;\n"
    "  public method Sum(): int\n"
    "    var s := 0;\n"
    "    for var x in _xs do s := s + x; end;\n"
    "    return s;\n"
    "  end Sum;\n"
    "end B;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var h := new B([1, 2, 3]);\n"
    "    Console.WriteLine(h.Sum());\n"
    "  end Main;\n"
    "end Program;\n";

// Remedy 2: the literal is bound to a `using var` declared BEFORE the
// holder, so the borrow source outlives the read and nothing is disposed
// early.
const char* kUsingVarHolder =
    "module Conf;\n"
    "public class B\n"
    "  private _xs: Array<int>;\n"
    "  public constructor(xs: Array<int>)\n"
    "    _xs := xs;\n"
    "  end;\n"
    "  public method Sum(): int\n"
    "    var s := 0;\n"
    "    for var x in _xs do s := s + x; end;\n"
    "    return s;\n"
    "  end Sum;\n"
    "end B;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    using var xs: Array<int> := [1, 2, 3];\n"
    "    var h := new B(xs);\n"
    "    Console.WriteLine(h.Sum());\n"
    "  end Main;\n"
    "end Program;\n";

// Remedy 3: `scoped` is not available for a collection-typed parameter
// (§28.14 admits it on a function-typed, class-typed, or interface-typed
// parameter only), so this shape is rejected at the DECLARATION.
const char* kScopedParam =
    "module Conf;\n"
    "public class B\n"
    "  private _xs: Array<int>;\n"
    "  public constructor(scoped xs: Array<int>)\n"
    "    _xs := xs;\n"
    "  end;\n"
    "  public method Sum(): int\n"
    "    var s := 0;\n"
    "    for var x in _xs do s := s + x; end;\n"
    "    return s;\n"
    "  end Sum;\n"
    "end B;\n"
    "public class Program\n"
    "  public static method Main()\n"
    "    var h := new B([1, 2, 3]);\n"
    "    Console.WriteLine(h.Sum());\n"
    "  end Main;\n"
    "end Program;\n";

std::string checkedOut(const std::string& src) {
    auto r = runAotCheckedDisposal(src);
    if (!r.ran) return "<did not run>";
    return r.stdoutText;
}

std::string defaultOut(const std::string& src) {
    auto r = runAot(src);
    if (!r.ran) return "<did not run>";
    return r.stdoutText;
}

} // anonymous namespace

// ── §28.3 clause 3 / §28.2 K4.1: the read through the stored borrow faults ─

BEZEL_TEST(codegen_collection_literal_stored_param_7045,
           spec_28_3_literal_to_plain_ctor_param_faults_after_boundary) {
    // §28.3 (v0.340) clause 3: the literal is an unbound owned temporary
    // passed to a plain parameter that the callee stores; it is NOT
    // lifetime-extended, so it is disposed at its boundary and the read
    // through the stored borrow afterwards is the backstop's fault.
    // §28.2 (v0.340) K4.1: in checked disposal mode that access raises
    // DisposedAccessException (§31.4).
    BEZEL_EXPECT_EQ(checkedOut(kExplicitCtor), "faulted\n");
}

BEZEL_TEST(codegen_collection_literal_stored_param_7045,
           spec_28_3_literal_to_plain_primary_ctor_param_faults_after_boundary) {
    // The primary constructor's synthesized capture field (§22.10) is the
    // same plain-parameter store as the explicit `_xs := xs`, so the read
    // through it faults the same way.
    BEZEL_EXPECT_EQ(checkedOut(kPrimaryCtor), "faulted\n");
}

BEZEL_TEST(codegen_collection_literal_stored_param_7045,
           spec_28_3_named_local_twin_faults_after_scope_end) {
    // §28.3 (v0.340) clause 3 names BOTH borrow sources: "an unbound owned
    // temporary OR a borrow of a scope-bound local". The local is disposed at
    // its scope's end, so the later read faults the same way — it must not
    // print the old value.
    BEZEL_EXPECT_EQ(checkedOut(kNamedLocalTwin), "faulted\n");
}

// ── the remedies the rule names must PRINT the values ──────────────────────

BEZEL_TEST(codegen_collection_literal_stored_param_7045,
           spec_28_3_owned_param_remedy_prints_value) {
    // §22.10 / §28.8: an `owned` parameter takes ownership, the field is not
    // a borrow, and the collection is disposed with the object — so the read
    // is defined in BOTH disposal modes.
    BEZEL_EXPECT_EQ(defaultOut(kOwnedParam), "6\n");
    BEZEL_EXPECT_EQ(checkedOut(kOwnedParam), "6\n");
}

BEZEL_TEST(codegen_collection_literal_stored_param_7045,
           spec_28_3_using_var_before_holder_remedy_prints_value) {
    // §28.3 clause 3's own remedy: bind the source to a named location whose
    // scope outlives the read. The `using var` is declared BEFORE the holder,
    // so the stored borrow is still live at the read.
    BEZEL_EXPECT_EQ(defaultOut(kUsingVarHolder), "6\n");
    BEZEL_EXPECT_EQ(checkedOut(kUsingVarHolder), "6\n");
}

// ── the `scoped` remedy does not apply to a collection-typed parameter ─────

BEZEL_TEST(codegen_collection_literal_stored_param_7045,
           spec_28_3_scoped_param_is_rejected_for_a_collection_parameter) {
    // §28.14 (v0.175/v0.180): `scoped` is valid only on a function-typed,
    // class-typed, or interface-typed parameter. A collection-typed parameter
    // cannot take it, so the declaration is rejected — the compiler must not
    // admit a shape the modifier does not cover.
    auto r = runAot(kScopedParam);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(!r.diagnostics.diagnostics().empty());
    std::string msg = r.diagnostics.diagnostics()[0].message;
    BEZEL_EXPECT_TRUE(msg.find("scoped") != std::string::npos);
    BEZEL_EXPECT_TRUE(msg.find("28.14") != std::string::npos);
    std::cout << "    (diagnostic: " << msg << ")\n";

}
