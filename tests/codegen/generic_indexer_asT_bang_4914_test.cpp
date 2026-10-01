// Codegen — issue #4914: `(k as T)!` inside a generic indexer getter fails
// to lower. Codegen silently dropped the value to null with the #2491
// "internal: return-value expression failed to lower to a value (degraded
// to a zero/null return value)" warning. The well-typed spelling (§17.4 `as`,
// §17.8 `!`) for "a bare-`T` element that aliases the string index, read via
// `this[...]`" must lower to a value: the getter must return the parameter's
// actual value (the alias `k`) so `this[$"x-{i}"]` reads back as the string
// the program stored. The §5.1 / §3.6 rule 2 (v0.307) reject of the bare
// `get => k;` spelling (#4910 / 64c0d712) left this row with no legal
// program; once `(k as T)!` lowers, the #2717 codegen pin
// (`expectLeakButNoMemoryError` + exit 71) on the issue's program text is
// restored.
//
// The probe at /var/tmp/bezel-scratch/r20/fx4910/p2717_as.bzl is the
// #2717 `kBareGenericElementViaThisResidual` program with `get => k;`
// replaced by `get => (k as T)!;`. The codegen path is the property-getter
// expression-body site in src/codegen/LlvmBackend_Assign.cpp (the `getterExpr`
// branch, which `boxForTarget`s the lowered value before `CreateRet`); the
// `(k as T)` lowering is `lowerTypeTest` in src/codegen/LlvmBackend_Layout.cpp,
// whose `rt` substitution (via `substituteTypeRefLocal` over
// `genericSubstSemaTy_`) is what makes the static-decision arm fire at the
// monomorphized body site where `T = string`. Pre-#4925 that substitution
// was absent, the bare-type-parameter target fell through, and `as` returned
// `null`.
//
// Two pins: the checked-AOT pin asserts the memory oracle ("leak but no
// memory error"), the plain-AOT pin asserts the value oracle (exit 71). The
// combined check mirrors the rest of this file's family: a leak is a #2717
// residual, the value comes back correct.
//
// Spec: §17.4 `as` (v0.307), §17.8 `!` (v0.307), §23.4 "Indexers" (v0.307),
// §5.1 / §3.6 rule 2 (v0.307). #4914.

#include <cstdio>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

std::string sanitizerText(const JitResult& r) {
    std::string all;
    for (const auto& d : r.diagnostics.diagnostics()) all += d.message + "\n";
    return all;
}

// A checked run that is expected to LEAK (the documented #2717 residual for
// an aliasing accessor) and, in particular, NOT to have freed anything early.
// Mirrors string_index_arg_release_2717_test.cpp's helper of the same name.
void expectLeakButNoMemoryError(const JitResult& r) {
    const std::string t = sanitizerText(r);
    BEZEL_EXPECT_TRUE(t.find("detected memory leaks") != std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("heap-use-after-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("double-free") == std::string::npos);
    BEZEL_EXPECT_TRUE(t.find("SEGV") == std::string::npos);
}

// sum over i in 1..20 of len("x-{i}") = 9*3 + 11*4 = 71.
constexpr int kLenSum20 = 71;

// The program text from #4914's issue body — the #2717
// `kBareGenericElementViaThisResidual` shape, with `get => k;` replaced by
// the well-typed spelling `get => (k as T)!;` (§17.4 + §17.8, v0.307). The
// rejection of the bare `get => k;` spelling (#4910 / 64c0d712) means this
// is the ONLY well-typed body that realises "a bare-`T` element that
// aliases the string index, read via `this[...]`" — §5.1 / §3.6 rule 2
// (v0.307) forbid the implicit `string -> T` conversion the bare spelling
// asks for, and #4810's reject is correct for that case. Once the `as T`
// target substitution lowers correctly (#4925 / 0de6b236), the returned
// value is the original `k` (a borrow, §17.4); the #2717 leak is the
// documented residual for an aliasing accessor.
const char* kAsTBangIndexerGetter =
    "module M; class Box<T> "
    "  public property [k: string]: T "
    "    get => (k as T)!; "
    "  end; "
    "  public method Get(i: int): T "
    "    return this[$\"x-{i}\"]; "
    "  end Get; "
    "end Box; "
    "class C "
    "  public static method Main(): int "
    "    using var b := new Box<string>(); "
    "    var total := 0; "
    "    for var i in 1..20 do "
    "      var s: string := b.Get(i); "
    "      var churn := $\"churn-churn-churn-{i}-{i}-{i}\"; "
    "      total := total + s.Length + churn.Length - churn.Length; "
    "    end; "
    "    return total; "                    // 71
    "  end Main; "
    "end C;";

}  // namespace

// =====================================================================
// (A) Memory oracle — the `(k as T)!` getter compiles, runs to completion,
// and is documented to LEAK per the #2717 residual. The fail-closed gate
// (`indexerElementCannotAliasIndex`) must NOT release the element; the
// well-typed body must NOT double-free. This row pins the leak-only verdict.
// =====================================================================
BEZEL_TEST(codegen_generic_indexer_asT_bang_4914,
           asT_bang_indexer_getter_leaks_but_no_memory_error_spec_17_4_17_8) {
    JitResult r = runAotChecked(kAsTBangIndexerGetter);
    if (wasSkipped(r)) return;
    expectLeakButNoMemoryError(r);
}

// =====================================================================
// (B) Value oracle — exit 71. The getter MUST return a non-null value
// (the `k` parameter alias), the `Get` method MUST pass it back as `T`,
// and the caller MUST read `b.Get(i)` as a non-null string of length
// 3 (for i = 1..9) or 4 (for i = 10..20). Pre-#4914 the getter degraded
// to null, `var s: string := b.Get(i)` raised NullReferenceException, and
// the program exited with a non-71 code; #4925 / 0de6b236 fixed the
// `lowerTypeTest` substitution so the static-decision arm fires for the
// monomorphized `T = string` site and the bare-parameter `as` target no
// longer falls through to a null return.
// =====================================================================
BEZEL_TEST(codegen_generic_indexer_asT_bang_4914,
           asT_bang_indexer_getter_value_is_correct_spec_17_4_17_8) {
    JitResult r = runAot(kAsTBangIndexerGetter);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kLenSum20);
}