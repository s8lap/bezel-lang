// Codegen + sema — issue #1723: stdlib §4.19 / §30.14 / §22.12 rule 4
// — the read-only-indexer rejection on FrozenArray<T>.
//
// §4.19 (v0.186): `FrozenArray<T>` is the deeply-immutable, sendable
// shared-read primitive. Its indexer is declared
//   public property [i: int]: T { get }
// with NO setter (the conformance authority is
// tests/conformance/cases/codegen_0028.bzl line 17); §30.14 freezes
// the value at construction so a programmatic mutation is a PROGRAM
// ERROR. Codegen's `lowerCollectionIndexerSet` (LlvmBackend_Assign.cpp
// around line 880) treats FrozenArray as a bz_array_t* — the same
// representation Array<T> uses — and SILENTLY routes `f[i] := v`
// through `bz_array_set`, mutating the storage. Pre-#1723 reads and
// with-tests pass; pre-#1723 writes ALSO pass and change the array,
// contradicting §4.19.
//
// The fix at sema level: `checkPropertyWriteTarget`
// (src/sema/ExprTypecheck.cpp line ~4740) is the single read/write
// gate for `:=`, `++`/`--`, and the compound `+=` family. The
// FrozenArray<T> receiver has TWO TypeRef shapes — the bare-annotation
// shape (`var f: FrozenArray<T> := ...`, headName ==
// "FrozenArray") and the static-factory result shape
// (`FrozenArray<T>.From(...)`, declaration ==
// builtinClassSym("FrozenArray"), headName() empty) — both caught.
//
// Cite: stdlib §4.19, spec §30.14 / §22.12 rule 4, v0.186+.
// Tests: 4 — 3 negative (`:=`, `++`, `+=`), 1 positive (read + iterate
// still compile cleanly; no regression in the read side that #3249
// landed).

#include "support/aot_support.hpp"   // runAot — AOT compile path
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

bool rejected(const std::string& program) {
    auto r = runAot(program);
    if (r.ran) return false;          // compiled — NOT rejected
    return r.diagnostics.hasErrors();
}

bool compiled(const std::string& program) {
    auto r = runAot(program);
    return r.ran && r.exitCode == 0;
}

int ran(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bool hasFrozenDiag(const bezel::test::JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("FrozenArray<T>") != std::string::npos &&
            d.message.find("deeply immutable") != std::string::npos)
            return true;
    return false;
}

}  // namespace

// ── §4.19 / §30.14 negative: a plain `f[0] := 5` on the static-factory
// result. This is the SHAPE that the pre-fix codegen silently mutated
// (the FrozenArray<int>.From(...) result type carries declaration ==
// builtinClassSym("FrozenArray") and headName() == ""; without the
// declaration-name disjunct this would NOT be caught).
BEZEL_TEST(frozen_array_1723, static_factory_indexer_set_rejected_spec_4_19) {
    const std::string program =
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var f := FrozenArray<int>.From([10, 20, 30]); "
        "    f[0] := 5; "
        "    return f[0]; "
        "end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);                                // NOT compiled
    BEZEL_EXPECT_TRUE(hasFrozenDiag(r));                      // right message
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());             // any error
}

// ── §4.19 / §30.14 negative: a pre-/post-increment on the indexer.
// §17.6 makes the indexer target the SAME storage category §23.3
// governs — "a writable field, an indexer slot, or a property with a
// setter" — so read-only indexers carry the same rejection under
// `++` / `--`. The fix threads `incOp` through `checkPropertyWriteTarget`
// so the diagnostic cites §17.6 alongside §4.19 / §30.14.
BEZEL_TEST(frozen_array_1723, increment_through_indexer_rejected_spec_17_6) {
    const std::string program =
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var f := FrozenArray<int>.From([10, 20, 30]); "
        "    f[0]++; "
        "    return f[0]; "
        "end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasFrozenDiag(r));
}

// ── §4.19 / §30.14 negative: a compound `+=` form. §23.3 routes
// `+=` through the same property-write-target check as `:=`, so the
// rejection applies — a write through `+=` is a write through `:=`.
BEZEL_TEST(frozen_array_1723, compound_assign_through_indexer_rejected_spec_23_3) {
    const std::string program =
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var f := FrozenArray<int>.From([10, 20, 30]); "
        "    f[0] += 5; "
        "    return f[0]; "
        "end Main; "
        "end C;";
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasFrozenDiag(r));
}

// ── §4.19 / §22.12 rule 4 positive: the read surface and the
// iteration surface are UNAFFECTED — they are the §4.19 read path
// (getter only). The existing `frozen_array_string_element_round_trip`
// (#3249) exercises `[i]` reads on FrozenArray<string>; this one
// exercises `[i]` reads on FrozenArray<int> plus `for ... in f`.
// If the fix accidentally over-rejects read paths, this fails — it
// pins the surface as read-only, never read-broken.
BEZEL_TEST(frozen_array_1723, read_and_iterate_paths_unchanged_spec_4_19) {
    const std::string program =
        "module M; "
        "public class C "
        "  public static method Main(): int "
        "    var f := FrozenArray<int>.From([1, 2, 3, 4]); "
        "    var sum := 0; "
        "    for var x in f do "
        "      sum := sum + x; "
        "    end; "
        "    return sum + f[0]; "          // 10 + 1 = 11
        "end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(ran(program), 11);
}
