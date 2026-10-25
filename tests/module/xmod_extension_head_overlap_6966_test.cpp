// fix #6966 (§26.4 / §26.5 / §26.5.1 / §2.8.2, v0.343) — a cross-module
// extension call must not silently bind a LESS SPECIFIC block's member.
//
// THE DEFECT. A library module declares
//
//     public extension Object  { public method Foo(): int => 1; }
//     public extension Box<T>  { public method Foo(): int => 2; }
//
// and an importer calls `b.Foo()` on a `Box<int>`. §26.5.1 ranks the
// APPLICABLE candidates by the specificity of their receiver-substituted
// heads: `Box<int>` is a subtype of `Object`, so the `Box<T>` block is more
// specific and WINS. The same-module program therefore prints 2.
//
// Cross-module, the `Box<T>` block used to be DROPPED from the artifact
// interface (its head binds a free type variable, so the exported symbol was
// compiled against the exporter's placeholder layout), leaving the importer
// with only the `Object` block — which it bound, printing 1 and exiting 0.
// A silent wrong answer across a module boundary.
//
// §2.8.2 ("No restrictions beyond visibility"): "An implementation unable to
// carry some body shape in its interface format must reject the export, or
// the import, with a diagnostic naming the shape; it must not alter the
// instantiation's semantics." So the import must be REJECTED, with a
// diagnostic that NAMES THE SHAPE (`extension Box<T>`) — not answered with
// some other block's member.
//
// WHY THE ROWS BELOW ARE TEN AND NOT ONE. The R34 attempt at this fix
// (commit 2fe5aa19d, REJECTED by the owner) withheld the exported member at
// EXPORT time, deciding from the member HEADS alone whether an unexported
// block could out-rank the exported one. That is receiver-blind, so it
// over-rejected five cross-module programs that are CORRECT — §26.5.1 ranks
// only the APPLICABLE candidates, and arity and a `where` clause are
// APPLICABILITY FILTERS applied BEFORE ranking, never ranking signals:
//
//   * unrelrecv      — the receiver is unrelated to the `Box<T>` competitor
//   * wherefalse     — the competitor's `where` clause is unsatisfied
//   * arity          — the competitor has a different arity
//   * objunrel       — the competitor is an unrelated generic extension
//   * base_supergen  — the receiver is the exported base
//
// Those five rows are ACCEPT rows here: the importer must print 1 on each,
// exactly as the same-module program does. The two REJECT rows (`orig`,
// `whereok`) are the ones the fix must reject. The two ACCEPT CONTROLS
// (`diffname`, `baseunrel_derived`) pin that an already-carried member is
// untouched. `ambig` is the MIRROR direction — a shape that is the ONLY
// applicable candidate, where the same-module program prints 2 and the
// importer cannot: see its own row.
//
// The oracle is `runAotModules` — the strict AOT path (fork-isolated
// Driver::compileFile + clang link + native exec), never the deprecated
// in-process JIT harness.

#include <string>

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::ModuleSource;
using bezel::test::diagnosticMessageContains;

namespace {

// The library's shared declarations. `Box<T>` is the receiver type; `Base` /
// `Derived` / `Other<T>` are the unrelated shapes the ACCEPT rows use.
const char* kTypes =
    "public class Box<T> public v: T; "
    "  public constructor(a: T) v := a; end constructor; end Box;\n"
    "public class Base public constructor() end constructor; end Base;\n"
    "public class Derived: Base public constructor() end constructor; "
    "end Derived;\n"
    "public class Other<T> public constructor() end constructor; end Other;\n";

// A cross-module program: `lib` is compiled as a library artifact, `app` is
// the importer.
JitResult xmod(const std::string& lib, const std::string& app) {
    return bezel::test::runAotModules(
        {ModuleSource{"L", "module L;\nimport System;\n" + lib},
         ModuleSource{"A", "module A;\nimport System;\nimport L;\n" + app}},
        "A");
}

// The SAME-MODULE control: one module holding both the library's declarations
// and the call. It is the answer the importer must agree with (§2.8.2 clause
// 2: an external instantiation "behaves identically to a same-module
// instantiation").
JitResult same(const std::string& lib, const std::string& app) {
    return bezel::test::runAotModules(
        {ModuleSource{"A", "module A;\nimport System;\n" + lib + "\n" + app}},
        "A");
}

// The importer must print `want` — the SAME value the same-module program
// prints (the ACCEPT rows).
void expectXmodPrints(const std::string& lib, const std::string& app,
                       const char* want) {
    JitResult r = xmod(lib, app);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, want);
}

// The importer must be REJECTED, and the diagnostic must NAME THE SHAPE —
// §2.8.2's "reject the export, or the import, with a diagnostic naming the
// shape". The shape named is the block whose head binds the free type
// variable (`extension Box<T>`), together with the module and the site.
void expectXmodRejectsNamingShape(const std::string& lib,
                                   const std::string& app,
                                   const char* shape) {
    JitResult r = xmod(lib, app);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, shape));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "§2.8.2"));
}

// The importer's program text: a `Box<int>` receiver calling `Foo()`.
const char* kBoxFooApp =
    "class P public static method Main(): int "
    "  var b := new Box<int>(1); "
    "  Console.WriteLine($\"{b.Foo()}\"); "
    "  return 0; end Main; end P;\n";

}  // namespace

// ---------------------------------------------------------------------------
// The REJECT rows — the defect itself.
// ---------------------------------------------------------------------------

// The issue's own shape: the `Box<T>` block out-ranks the `Object` block for
// a `Box<int>` receiver (§26.5.1), so the importer must not answer with the
// `Object` block's 1. It must reject, naming `extension Box<T>`.
//
// FAILS WITHOUT THE FIX: the importer binds the `Object` block, prints 1 and
// exits 0 — no diagnostic at all.
BEZEL_TEST(xmod_extension_head_overlap,
           free_variable_head_outranks_object_block_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(): int => 1; end;\n"
        "public extension Box<T> public method Foo(): int => 2; end;\n";
    // The same-module program binds the `Box<T>` block and prints 2 — the
    // answer the importer must not contradict.
    JitResult s = same(lib, kBoxFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, kBoxFooApp, "extension Box<T>");
}

// The same shape with a `where` clause on the competitor: §26.5.1 calls a
// `where` clause an APPLICABILITY FILTER, so at an `int` argument (which
// satisfies `IComparable<int>`) the `Box<T>` block still applies and still
// out-ranks `Object` — the importer must reject, naming the shape.
BEZEL_TEST(xmod_extension_head_overlap,
           constrained_free_variable_head_rejected_naming_shape_2_8_2) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(): int => 1; end;\n"
        "public extension Box<T> where T: IComparable<T> "
        "  public method Foo(): int => 2; end;\n";
    JitResult s = same(lib, kBoxFooApp);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    expectXmodRejectsNamingShape(lib, kBoxFooApp, "extension Box<T>");
}

// ---------------------------------------------------------------------------
// The ACCEPT rows — the R34 attempt over-rejected every one of these. They
// are the rows that keep a fix from becoming a receiver-blind export-time
// withhold. §26.5.1: specificity ranks the APPLICABLE candidates; a `where`
// clause is an applicability filter, never a ranking signal.
// ---------------------------------------------------------------------------

// The receiver is unrelated to the `Box<T>` competitor: `Derived` is not a
// `Box`, so the competitor is not even a candidate and the `Object` block
// applies. The importer must print 1.
BEZEL_TEST(xmod_extension_head_overlap,
           unrelated_receiver_keeps_exported_member_26_5_1) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(): int => 1; end;\n"
        "public extension Box<T> public method Foo(): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var d := new Derived(); "
        "  Console.WriteLine($\"{d.Foo()}\"); "
        "  return 0; end Main; end P;\n";
    expectXmodPrints(lib, app, "1\n");
}

// The competitor's `where` clause is UNSATISFIED at the receiver's
// instantiation (`Base` does not implement `IComparable<Base>`), so the
// competitor does not apply at all (§26.5.1's applicability filter) and the
// `Object` block answers. The importer must print 1 — not reject.
BEZEL_TEST(xmod_extension_head_overlap,
           unsatisfied_where_clause_keeps_exported_member_26_5_1) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(): int => 1; end;\n"
        "public extension Box<T> where T: IComparable<T> "
        "  public method Foo(): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var b := new Box<Base>(new Base()); "
        "  Console.WriteLine($\"{b.Foo()}\"); "
        "  return 0; end Main; end P;\n";
    expectXmodPrints(lib, app, "1\n");
}

// The competitor has a DIFFERENT ARITY (`Foo(x: int)` against a `Foo()` call),
// so it is not an applicable candidate (§22.6's arity filter) and the
// `Object` block answers. The importer must print 1 — not reject.
BEZEL_TEST(xmod_extension_head_overlap,
           different_arity_keeps_exported_member_22_6) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(): int => 1; end;\n"
        "public extension Box<T> public method Foo(x: int): int => 2; end;\n";
    expectXmodPrints(lib, kBoxFooApp, "1\n");
}

// The competitor is an unrelated GENERIC extension (`extension Other<T>`) and
// the receiver is `Derived`: the competitor's head does not match the
// receiver, so the `Object` block answers. The importer must print 1.
BEZEL_TEST(xmod_extension_head_overlap,
           unrelated_generic_competitor_keeps_exported_member_26_5_1) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(): int => 1; end;\n"
        "public extension Other<T> public method Foo(): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var d := new Derived(); "
        "  Console.WriteLine($\"{d.Foo()}\"); "
        "  return 0; end Main; end P;\n";
    expectXmodPrints(lib, app, "1\n");
}

// The receiver is the EXPORTED BASE (`Base`), and the competitor is a
// generic extension over a SUBCLASS of it (`extension Gen<T> : Base`): the
// competitor's head does not match a `Base` receiver, so the `Base` block
// answers. The importer must print 1.
BEZEL_TEST(xmod_extension_head_overlap,
           exported_base_receiver_keeps_exported_member_26_5_1) {
    const std::string lib =
        "public class Base public constructor() end constructor; end Base;\n"
        "public class Gen<T>: Base public constructor() end constructor; "
        "end Gen;\n"
        "public extension Base public method Foo(): int => 1; end;\n"
        "public extension Gen<T> public method Foo(): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var b := new Base(); "
        "  Console.WriteLine($\"{b.Foo()}\"); "
        "  return 0; end Main; end P;\n";
    expectXmodPrints(lib, app, "1\n");
}

// ---------------------------------------------------------------------------
// The ACCEPT CONTROLS — an already-carried member is untouched.
// ---------------------------------------------------------------------------

// The competitor declares a DIFFERENT member name (`Bar`), so it is not a
// candidate for `Foo` at all and the `Object` block answers. The importer
// must print 1.
BEZEL_TEST(xmod_extension_head_overlap,
           different_member_name_keeps_exported_member_26_5) {
    const std::string lib =
        std::string(kTypes) +
        "public extension Object public method Foo(): int => 1; end;\n"
        "public extension Box<T> public method Bar(): int => 2; end;\n";
    expectXmodPrints(lib, kBoxFooApp, "1\n");
}

// Two DISPATCHABLE blocks, both ground heads: `extension Base` and
// `extension Derived`. The receiver is `Base`, so `Derived`'s head does not
// match it and `Base` answers. The importer must print 1 — the shape
// carriage must not disturb blocks that were already carried.
BEZEL_TEST(xmod_extension_head_overlap,
           two_ground_blocks_receiver_base_keeps_its_own_26_5_1) {
    const std::string lib =
        "public class Base public constructor() end constructor; end Base;\n"
        "public class Derived: Base public constructor() end constructor; "
        "end Derived;\n"
        "public extension Base public method Foo(): int => 1; end;\n"
        "public extension Derived public method Foo(): int => 3; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var b := new Base(); "
        "  Console.WriteLine($\"{b.Foo()}\"); "
        "  return 0; end Main; end P;\n";
    expectXmodPrints(lib, app, "1\n");
}

// ---------------------------------------------------------------------------
// The MIRROR direction (`ambig`) — a shape that is the ONLY applicable
// candidate.
//
// The library declares `extension Base { Foo }` and `extension Other<T> { Foo }`;
// the importer calls `o.Foo()` on an `Other<int>`. The same-module program
// binds the `Other<T>` block and prints 2. Cross-module the `Other<T>` shape
// cannot be dispatched at all (the same free-variable head as the REJECT
// rows), so the importer REJECTS — with the shape-naming diagnostic, not
// with a silent binding of the `Base` block's 1.
//
// This row records the RESIDUAL limitation honestly: the importer cannot
// ANSWER 2 here, because answering it needs importer-side monomorphization
// of the free-variable head (§26.4's #783 machinery), which does not exist
// for extension blocks. What the fix guarantees is that the importer never
// answers a DIFFERENT question (the `Base` block's 1).
// ---------------------------------------------------------------------------
BEZEL_TEST(xmod_extension_head_overlap,
           mirror_direction_shape_only_candidate_rejected_not_silently_bound_2_8_2) {
    const std::string lib =
        "public class Base public constructor() end constructor; end Base;\n"
        "public class Other<T> public constructor() end constructor; "
        "end Other;\n"
        "public extension Base public method Foo(): int => 1; end;\n"
        "public extension Other<T> public method Foo(): int => 2; end;\n";
    const char* app =
        "class P public static method Main(): int "
        "  var o := new Other<int>(); "
        "  Console.WriteLine($\"{o.Foo()}\"); "
        "  return 0; end Main; end P;\n";
    JitResult s = same(lib, app);
    BEZEL_EXPECT_TRUE(s.ran);
    BEZEL_EXPECT_EQ(s.exitCode, 0);
    BEZEL_EXPECT_EQ(s.stdoutText, "2\n");

    // The importer must NOT answer 1 (the `Base` block's value).
    JitResult r = xmod(lib, app);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "extension Other<T>"));
}
