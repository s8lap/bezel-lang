// fix #3023 (§11.2 / §14.1 / §2.8.3 rule 5, v0.240): a USER interface
// whose bare NAME collides with a synthesized builtin interface
// (IComparable / IEquatable / IEqualityComparer / IReadOnlySpan / INumber /
// ISteppable / IParsable — `isSynthesizedBuiltinInterfaceName`,
// SemaContext.hpp:602) must compile cleanly. Before this fix, declaring e.g.
// `interface IReadOnlySpan<T>` in a module alongside a class SEGFAULT'd the
// compiler (`bezelc` exit 139 / SIGSEGV) at declareVtables Pass B.
//
// Root cause: declareVtables Pass B (LlvmBackend_Setup.cpp:3871, the
// `synthesizedBuiltinIface` predicate) detected builtin-ness by IDENTITY
// against `ctx_.builtinInterfaceSym(ts->name)`. The CREATING form mints a new
// synthesized Interface TypeSymbol on a cache miss and `push_back`s it onto
// `ctx_.symbols()` — which is the very vector the OUTER
// `for (const auto& sym : ctx_.symbols())` loop is iterating. With a cold
// cache (no sema-side mention of the builtin name) and the user's interface
// declaration in hand, the push_back reallocated `symbols_`, invalidating the
// loop's iterators and silently dropping later symbols — including the
// user's class C. `descArray_[C]` was therefore never populated, and Pass D's
// `elems[0] = descArray_[ts]` handed `llvm::ConstantArray::get` a null
// `llvm::Constant*` to dereference → SIGSEGV. (Probe trace on the WIP branch,
// commit 7a0f603, before the fix: the very first user `interface IReadOnlySpan<T>`
// declaration emits ~8 new builtin symbols mid-iteration and the class
// descriptor is silently skipped.)
//
// Fix: a non-creating `builtinInterfaceSymIfCached` lookup that returns
// `nullptr` on a cache miss, never minting. The identity check then correctly
// fails for the user's interface (cache cold → returns nullptr → `ts != nullptr`
// predicate is false → `synthesizedBuiltinIface` is false → the user interface
// gets ordinary internal/external linkage, NOT the synthesized builtin's
// linkonce_odr). #2776's ODR guarantee for the synthesized builtin itself
// (which IS in the cache whenever it's been used in this module) is unchanged.
//
// All seven `isSynthesizedBuiltinInterfaceName` names are exercised below —
// the predicate's *closed set* is the only place that could plausibly behave
// differently, and each must compile + link + run independently.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

// Each program declares exactly one USER interface whose bare name collides
// with a synthesized builtin, plus a class with Main that returns a sentinel
// int (so the test can assert exit-code identity and prove the user's class
// descriptor was actually emitted). A failing compile (SIGSEGV in bezelc)
// surfaces via runAot's forked-compile isolation: `r.ran == false` and the
// diagnostic carries "compiler CRASHED" (aot_support.hpp:803-815).
const char* kUserIReadOnlySpan =
    "module M; "
    "interface IReadOnlySpan<T> "
    "  method Len(): int; "
    "end IReadOnlySpan; "
    "class C public static method Main(): int return 7; end Main; end C;";

const char* kUserIComparable =
    "module M; "
    "interface IComparable<T> "
    "  method CompareTo(other: T): int; "
    "end IComparable; "
    "class C public static method Main(): int return 11; end Main; end C;";

const char* kUserIEquatable =
    "module M; "
    "interface IEquatable<T> "
    "  method Equals(other: T): bool; "
    "end IEquatable; "
    "class C public static method Main(): int return 13; end Main; end C;";

const char* kUserIEqualityComparer =
    "module M; "
    "interface IEqualityComparer<T> "
    "  method Eq(a: T, b: T): bool; "
    "  method Hc(value: T): int; "
    "end IEqualityComparer; "
    "class C public static method Main(): int return 17; end Main; end C;";

const char* kUserINumber =
    "module M; "
    "interface INumber<T> "
    "  method Zero(): int; "
    "end INumber; "
    "class C public static method Main(): int return 19; end Main; end C;";

const char* kUserISteppable =
    "module M; "
    "interface ISteppable<T> "
    "  method Step(value: T, by: long): int; "
    "end ISteppable; "
    "class C public static method Main(): int return 23; end Main; end C;";

const char* kUserIParsable =
    "module M; "
    "interface IParsable<T> "
    "  method Parse(s: string): int; "
    "end IParsable; "
    "class C public static method Main(): int return 29; end Main; end C;";

}  // namespace

// ── axis-grid: each closed-set builtin-name compiles + runs ────────────────
// Pre-fix (HEAD 5995756) every one of these SIGSEGV'd bezelc inside
// declareVtables Pass B; post-fix each returns its sentinel exit code.

BEZEL_TEST(codegen_user_iface_builtin_name_decl_3023,
           user_ireadonlyspan_decl_no_segfault) {
    auto r = runAot(kUserIReadOnlySpan);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

BEZEL_TEST(codegen_user_iface_builtin_name_decl_3023,
           user_icomparable_decl_no_segfault) {
    auto r = runAot(kUserIComparable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 11);
}

BEZEL_TEST(codegen_user_iface_builtin_name_decl_3023,
           user_iequatable_decl_no_segfault) {
    auto r = runAot(kUserIEquatable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

BEZEL_TEST(codegen_user_iface_builtin_name_decl_3023,
           user_iequalitycomparer_decl_no_segfault) {
    auto r = runAot(kUserIEqualityComparer);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 17);
}

BEZEL_TEST(codegen_user_iface_builtin_name_decl_3023,
           user_inumber_decl_no_segfault) {
    auto r = runAot(kUserINumber);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 19);
}

BEZEL_TEST(codegen_user_iface_builtin_name_decl_3023,
           user_isteppable_decl_no_segfault) {
    auto r = runAot(kUserISteppable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 23);
}

BEZEL_TEST(codegen_user_iface_builtin_name_decl_3023,
           user_iparsable_decl_no_segfault) {
    auto r = runAot(kUserIParsable);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 29);
}

// ── combined: all seven user interfaces + one class (the densest iteration
//    variant — each user's `isSynthesizedBuiltinInterfaceName` predicate fires
//    in Pass B, with N reallocation opportunities chained instead of one).

BEZEL_TEST(codegen_user_iface_builtin_name_decl_3023,
           all_seven_builtin_names_in_one_module) {
    auto r = runAot(
        "module M; "
        "interface IComparable<T> method CompareTo(other: T): int; end IComparable; "
        "interface IEquatable<T> method Equals(other: T): bool; end IEquatable; "
        "interface IEqualityComparer<T> "
        "  method Eq(a: T, b: T): bool; method Hc(value: T): int; "
        "end IEqualityComparer; "
        "interface IReadOnlySpan<T> method Len(): int; end IReadOnlySpan; "
        "interface INumber<T> method Zero(): int; end INumber; "
        "interface ISteppable<T> method Step(value: T, by: long): int; end ISteppable; "
        "interface IParsable<T> method Parse(s: string): int; end IParsable; "
        "class C public static method Main(): int return 31; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 31);
}