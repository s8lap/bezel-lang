// Spec-derived tests for fix #4038 — §27.8 (v0.294): a module-level `type`
// alias used as the RECEIVER of a static-member access (`AliasOfFoo.Bar`) did
// not see through to the aliased type. §27.8, verbatim: "Type aliases — generic
// or not — see through to the aliased type for identity, equality, method
// dispatch, and overload resolution." Static member access is exactly such a
// dispatch site.
//
// This is a SILENT WRONG-ANSWER bug, not a rejection — worse than a crash,
// because the program compiles and runs with no diagnostic naming the defect:
//
//   * READ: `resolveMemberAccess` (ExprTypecheck_Calls.cpp) never attached the
//     resolved member symbol to the MemberAccessExpr — a TypeAliasSymbol
//     receiver fell through every existing arm (TypeSymbol / NestedTypeSymbol /
//     LocalSymbol / ParameterSymbol / GenericParamSymbol), leaving `targetType`
//     null. Codegen's `lowerExpr` then had nothing to lower, and the return
//     statement's own #2491 backstop (LlvmBackend_Stmts.cpp) substituted a
//     defined zero with a loud "internal: return-value expression failed to
//     lower to a value ... — #2491" Warning. That backstop is doing its job —
//     it exists so a codegen lowering gap degrades to a labeled zero rather
//     than `undef` or a crash — but the actual defect is the sema resolution
//     gap one layer up; this fix closes that gap so the backstop has nothing
//     to catch for this shape (its own #2491 tests are untouched).
//   * WRITE: with no FieldSymbol attached to the MemberAccessExpr, codegen's
//     `lvalueAddr` (LlvmBackend_Layout.cpp) found no local slot, no instance
//     field address, and no static-field global (`staticFieldGlobal` keys off
//     `ctx_.exprSymbol()` of the WHOLE MemberAccessExpr, never set here). The
//     generic `:=` fallback (LlvmBackend_Stmts.cpp) then hit
//     `addr = lvalueAddr(...); if (!addr) return false;` with **no diagnostic
//     of any severity** — the write silently no-ops and the RHS is never even
//     evaluated.
//
// The fix adds a `TypeAliasSymbol` arm to `resolveMemberAccess`'s target-type
// resolution (ExprTypecheck_Calls.cpp): it resolves the alias's `aliasedType`
// (already eagerly resolved by `Resolver::resolveTopDecl` when the `type`
// declaration itself was processed — the same fact the type-POSITION alias-
// transparency arm in TypeResolution_Names.cpp, fix #578, already reuses) and
// treats a Named underlying type exactly like a direct TypeSymbol receiver.
// A second, one-line fix was needed in ExprTypecheck_Exprs.cpp's §22.12
// instance-vs-type-name receiver check: it recognized a `TypeSymbol` or
// `GenericParamSymbol` receiver as a "type name" but not a `TypeAliasSymbol`
// one, so once the primary fix started giving the alias identifier a real
// exprType, this second check newly (and wrongly) rejected the legal
// type-name access as "must be accessed through the type name, not an
// instance (§22.12)".
//
// ── GRID ─────────────────────────────────────────────────────────────────
//
// Covered here (see #4038 for the full grid explored in the fix's own
// investigation, including a two-module `--import` cross-module cell not
// practical to embed in a single-source runAot test):
//   * class static field:      read / write / compound-assign / increment
//   * class static auto-property: read / write
//   * class static method call
//   * class static const
//   * record static field, static const, static method (records support
//     static members per §9.2 "Static members are ignored" for the
//     auto-equality census, but are ordinary static members otherwise)
//   * alias of a CLOSED generic instantiation (`type A := G<int>;`)
//   * alias of a NESTED type (`type A := Outer.Inner;`)
//   * chained alias-of-alias, INNER DECLARED BEFORE OUTER
//     (`type Inner := SomeClass; type Outer := Inner;`, inner textually
//     FIRST) — FIXED. The REVERSE declaration order (`type Outer := Inner;
//     type Inner := SomeClass;`, outer textually first) is a SEPARATE,
//     pre-existing, upstream bug this fix does not touch — see #4047 below
//     and its pinning control test.
//   * anti-regression: instance-member access via an alias-typed local
//     (`var x: AliasOf := new C(); x.Field`) — this path was ALREADY correct
//     pre-fix (it resolves via TypeResolution_Names.cpp's TYPE-POSITION alias
//     transparency, fix #578, a different code path from this fix's target)
//   * anti-regression: the non-alias direct-name form for both the read and
//     write repros, pinning the fix did not disturb the working baseline

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// #4038 issue repro, read half, verbatim (§27.8): a `readonly` static field
// read through a type alias degraded to zero (the #2491 guard fired) instead
// of reading the real storage. "init".Length == 4.
BEZEL_TEST(type_alias_static_member_4038, read_static_field_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasOfQ14B := StaticClassQ14B; "
        "public class StaticClassQ14B "
        "  public static readonly Shared: string := \"init\"; "
        "end StaticClassQ14B; "
        "class C "
        "  public static method Main(): int "
        "    return AliasOfQ14B.Shared.Length; "
        "  end Main; "
        "end C;"), 4);
}

// #4038 issue repro, write half, verbatim (§27.8): a plain static field write
// through a type alias silently no-op'd (no diagnostic at any severity) —
// the value read back through the REAL name was 0, not the written 42.
BEZEL_TEST(type_alias_static_member_4038, write_static_field_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasOfQ14C := StaticClassQ14C; "
        "public class StaticClassQ14C "
        "  public static Counter: int; "
        "end StaticClassQ14C; "
        "class C "
        "  public static method Main(): int "
        "    AliasOfQ14C.Counter := 42; "
        "    return StaticClassQ14C.Counter; "
        "  end Main; "
        "end C;"), 42);
}

// §27.8: reading BACK through the SAME alias must also see the write (not
// just the direct name, pinned above) — the alias and the direct name name
// the same storage.
BEZEL_TEST(type_alias_static_member_4038, write_and_read_back_through_same_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasSame := StaticClassSame; "
        "public class StaticClassSame "
        "  public static Counter: int; "
        "end StaticClassSame; "
        "class C "
        "  public static method Main(): int "
        "    AliasSame.Counter := 42; "
        "    return AliasSame.Counter; "
        "  end Main; "
        "end C;"), 42);
}

// §27.8 / §17.6: compound assignment (`+=`) through an alias must
// read-modify-write the real storage.
BEZEL_TEST(type_alias_static_member_4038, compound_assign_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasComp := StaticClassComp; "
        "public class StaticClassComp "
        "  public static Counter: int; "
        "end StaticClassComp; "
        "class C "
        "  public static method Main(): int "
        "    AliasComp.Counter := 10; "
        "    AliasComp.Counter += 5; "
        "    return StaticClassComp.Counter; "
        "  end Main; "
        "end C;"), 15);
}

// §27.8 / §17.6: increment (`++`) through an alias.
BEZEL_TEST(type_alias_static_member_4038, increment_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasIncr := StaticClassIncr; "
        "public class StaticClassIncr "
        "  public static Counter: int; "
        "end StaticClassIncr; "
        "class C "
        "  public static method Main(): int "
        "    AliasIncr.Counter := 10; "
        "    AliasIncr.Counter++; "
        "    return StaticClassIncr.Counter; "
        "  end Main; "
        "end C;"), 11);
}

// §23.1 / §27.8: a static AUTO-property (backing global, fix #743) read and
// written through an alias.
BEZEL_TEST(type_alias_static_member_4038, static_autoproperty_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasProp := StaticClassProp; "
        "public class StaticClassProp "
        "  public static property X: int; "
        "end StaticClassProp; "
        "class C "
        "  public static method Main(): int "
        "    AliasProp.X := 7; "
        "    return AliasProp.X; "
        "  end Main; "
        "end C;"), 7);
}

// §22.12 / §27.8: a static METHOD called through an alias.
BEZEL_TEST(type_alias_static_member_4038, static_method_call_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasMeth := StaticClassMeth; "
        "public class StaticClassMeth "
        "  public static method Double(n: int): int => n * 2; "
        "end StaticClassMeth; "
        "class C "
        "  public static method Main(): int "
        "    return AliasMeth.Double(21); "
        "  end Main; "
        "end C;"), 42);
}

// §22.12 / §27.8: a class-level `const` read through an alias.
BEZEL_TEST(type_alias_static_member_4038, class_const_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasConst := StaticClassConst; "
        "public class StaticClassConst "
        "  public const K: int := 42; "
        "end StaticClassConst; "
        "class C "
        "  public static method Main(): int "
        "    return AliasConst.K; "
        "  end Main; "
        "end C;"), 42);
}

// §9.2 / §22.12 / §27.8: RECORDS support static members too ("Static members
// are ignored" for the record's own auto-equality census, §9.2 — they are
// ordinary static members otherwise). A static field write/read through an
// alias of a RECORD, not a class.
BEZEL_TEST(type_alias_static_member_4038, record_static_field_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasRec := RecStatic4038; "
        "public record RecStatic4038(V: int) "
        "  public static Counter: int; "
        "end RecStatic4038; "
        "class C "
        "  public static method Main(): int "
        "    AliasRec.Counter := 41; "
        "    return RecStatic4038.Counter; "
        "  end Main; "
        "end C;"), 41);
}

// §9.2 / §27.8: a record's static method called through an alias.
BEZEL_TEST(type_alias_static_member_4038, record_static_method_through_alias) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasRecM := RecStaticM4038; "
        "public record RecStaticM4038(V: int) "
        "  public static method Triple(n: int): int => n * 3; "
        "end RecStaticM4038; "
        "class C "
        "  public static method Main(): int "
        "    return AliasRecM.Triple(7); "
        "  end Main; "
        "end C;"), 21);
}

// §22.12 / §27.8: an alias of a CLOSED generic instantiation
// (`type A := G<int>;`). §22.12, verbatim: "Each distinct instantiation has
// its own static state ([§22.13])"; the alias already carries the concrete
// type argument, so no explicit `<int>` is written (or needed) at the alias
// use site.
BEZEL_TEST(type_alias_static_member_4038, generic_closed_alias_static_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasGeneric4038 := GenericClass4038<int>; "
        "public class GenericClass4038<T> "
        "  public static Counter: int; "
        "end GenericClass4038; "
        "class C "
        "  public static method Main(): int "
        "    AliasGeneric4038.Counter := 55; "
        "    return GenericClass4038<int>.Counter; "
        "  end Main; "
        "end C;"), 55);
}

// §11.6 / §27.8: an alias of a NESTED type.
BEZEL_TEST(type_alias_static_member_4038, nested_type_alias_static_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class Outer4038 "
        "  public class Inner4038 "
        "    public static Counter: int; "
        "    destructor end destructor; "
        "  end Inner4038; "
        "  destructor end destructor; "
        "end Outer4038; "
        "type AliasNested4038 := Outer4038.Inner4038; "
        "class C "
        "  public static method Main(): int "
        "    AliasNested4038.Counter := 33; "
        "    return Outer4038.Inner4038.Counter; "
        "  end Main; "
        "end C;"), 33);
}

// §27.8: "Aliases of aliases are permitted" — a chained alias-of-alias must
// see through BOTH hops to the real static storage.
//
// ⚠ This FIXED verdict holds ONLY for this declaration order — the inner
// alias (`AliasChainInner4038`) declared TEXTUALLY BEFORE the outer one
// (`AliasChainOuter4038`) that names it. The REVERSE order is a separate,
// pre-existing, upstream bug (#4047) this fix does not touch — see the
// `chained_alias_of_alias_REVERSE_ORDER_known_open_4047` control below.
BEZEL_TEST(type_alias_static_member_4038, chained_alias_of_alias_static_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class StaticClassChain4038 "
        "  public static Counter: int; "
        "end StaticClassChain4038; "
        "type AliasChainInner4038 := StaticClassChain4038; "
        "type AliasChainOuter4038 := AliasChainInner4038; "
        "class C "
        "  public static method Main(): int "
        "    AliasChainOuter4038.Counter := 17; "
        "    return StaticClassChain4038.Counter; "
        "  end Main; "
        "end C;"), 17);
}

// ── REGRESSION for #4047 (§27.8, v0.294) ─────────────────────────────────
//
// §27.8: "Aliases of aliases are permitted" places no ordering requirement
// on a chained alias declaration — `type Outer := Inner; type Inner :=
// SomeClass;` (outer declared FIRST, inner declared second) must resolve
// exactly like the reverse order pinned above.
//
// Root cause (filed during review of #4038): `Resolver::resolveTopDecl`
// (TypeResolution.cpp) resolves each top-level `type` alias's `aliasedType`
// in a single forward pass over `src.topDecls` in file declaration order.
// When `Outer` is declared before `Inner`, resolving `Outer`'s aliased
// type (a reference to `Inner`) runs BEFORE `Inner` itself has been
// resolved, so fix #578's alias-transparency arm (TypeResolution_Names.cpp)
// sees a null `ctx_.resolvedType(...)` for `Inner` and falls through to
// the "Named, no declaration" silent-unresolved state (#4047's own
// mechanism — pre-existing, predates #4038 and reproduces byte-for-byte
// on the pre-#4038 parent commit). The fix #4038 TypeAliasSymbol arm in
// ExprTypecheck_Calls.cpp inherits the same null through its identical
// `ctx_.resolvedType(*aliasDecl->aliasedType)` lookup, so the write-side
// silent-no-op and the read-side #2491-degraded-to-0 symptoms both
// reproduce verbatim on the reverse declaration order.
//
// Fix (#4047, §27.8/v0.294): after the forward pass over top decls,
// `Resolver::resolveAliasForwardReferences` walks every top-level alias
// and re-resolves any whose `aliasedType` came back as `Kind::Named` with
// `declaration == nullptr`, iterating to a fixed point so chains of
// arbitrary depth resolve in one sweep. A user-written alias cycle
// (A := B; B := A) is not chartered for this fix — the iteration cap makes
// it deterministic, and the resulting still-unresolved state surfaces as
// the same downstream diagnostic it does today.
BEZEL_TEST(type_alias_static_member_4038,
           chained_alias_of_alias_REVERSE_ORDER_4047) {
    // FIXED (#4047): was 0, now 17 — matches the inner-before-outer order
    // pinned above. The write through `AliasChainOuterRev4047.Counter` now
    // reaches the real `StaticClassChainRev4047.Counter` storage instead of
    // silently no-opping through the unresolved alias.
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class StaticClassChainRev4047 "
        "  public static Counter: int; "
        "end StaticClassChainRev4047; "
        "type AliasChainOuterRev4047 := AliasChainInnerRev4047; "
        "type AliasChainInnerRev4047 := StaticClassChainRev4047; "
        "class C "
        "  public static method Main(): int "
        "    AliasChainOuterRev4047.Counter := 17; "
        "    return StaticClassChainRev4047.Counter; "
        "  end Main; "
        "end C;"), 17);
}

// Regression for #4047, read half: the same reverse-order shape USED to
// hit the literal #2491 zero-lowering backstop on READ (exactly like
// #4038's own read repro did pre-fix). Now it returns the real length.
BEZEL_TEST(type_alias_static_member_4038,
           chained_alias_of_alias_REVERSE_ORDER_read_4047) {
    auto r = runAot(
        "module M; "
        "public class StaticClassChainRevRead4047 "
        "  public static readonly Shared: string := \"hi\"; "
        "end StaticClassChainRevRead4047; "
        "type AliasChainOuterRevRead4047 := AliasChainInnerRevRead4047; "
        "type AliasChainInnerRevRead4047 := StaticClassChainRevRead4047; "
        "class C "
        "  public static method Main(): int "
        "    return AliasChainOuterRevRead4047.Shared.Length; "
        "  end Main; "
        "end C;");
    // FIXED (#4047): was a #2491-degraded 0, now 2 ("hi".Length).
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// Anti-regression: instance-member access via an alias-TYPED local variable
// was ALREADY correct pre-#4038 (a different code path — TYPE POSITION alias
// transparency in TypeResolution_Names.cpp, fix #578). Pins the #4038 fix did
// not disturb it.
BEZEL_TEST(type_alias_static_member_4038, instance_member_via_alias_typed_local_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "type AliasInst4038 := StaticClassInst4038; "
        "public class StaticClassInst4038 "
        "  public _v: int; "
        "  public constructor(v: int) "
        "    _v := v; "
        "  end constructor; "
        "  destructor end destructor; "
        "end StaticClassInst4038; "
        "class C "
        "  public static method Main(): int "
        "    using var x: AliasInst4038 := new StaticClassInst4038(77); "
        "    return x._v; "
        "  end Main; "
        "end C;"), 77);
}

// Anti-regression: the DIRECT (non-alias) form of both #4038 repros must
// still produce the correct value — pins the fix's new TypeAliasSymbol arm
// in resolveMemberAccess did not change the pre-existing plain-TypeSymbol
// arm's behavior.
BEZEL_TEST(type_alias_static_member_4038, direct_name_read_and_write_unaffected) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "public class StaticClassDirect4038 "
        "  public static Counter: int; "
        "  public static readonly Shared: string := \"init\"; "
        "end StaticClassDirect4038; "
        "class C "
        "  public static method Main(): int "
        "    StaticClassDirect4038.Counter := 42; "
        "    return StaticClassDirect4038.Counter + "
        "           StaticClassDirect4038.Shared.Length; "
        "  end Main; "
        "end C;"), 46);
    // 42 + "init".Length(4) == 46
}

// Diagnostic-hygiene anti-regression: pre-fix, the read repro compiled with
// NO ordinary diagnostic and only the internal #2491 backstop Warning on
// stderr; post-fix, `lowerExpr` succeeds and the backstop never fires. Assert
// its distinctive text is ABSENT from stdout+stderr capture on the fixed
// compiler (runAot's `stdoutText` does not include the compiler's own
// diagnostics, but a `hasErrors()`-style failure would prevent `r.ran` from
// being true at all — the real oracle here is the VALUE, asserted above; this
// pins the diagnostic-free compile as an explicit, named expectation).
BEZEL_TEST(type_alias_static_member_4038, read_through_alias_runs_without_2491_backstop) {
    auto r = runAot(
        "module M; "
        "type AliasNoBackstop4038 := StaticClassNoBackstop4038; "
        "public class StaticClassNoBackstop4038 "
        "  public static readonly Shared: string := \"hello\"; "
        "end StaticClassNoBackstop4038; "
        "class C "
        "  public static method Main(): int "
        "    return AliasNoBackstop4038.Shared.Length; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);
}

// ── §15 UNDER-TESTED-DIRECTION SWEEP for the ExprTypecheck_Exprs.cpp
//    companion fix ─────────────────────────────────────────────────────────
//
// The companion fix widens `recvIsType` (the §22.12 "static member must be
// accessed through the TYPE NAME, not an instance" check's receiver
// classifier) to also accept a `TypeAliasSymbol` receiver. Per lane
// discipline (§15), a false-reject-REMOVAL fix is under-tested on the
// "still correctly rejects" direction — the two cases below pin that this
// widening did NOT also let an actual INSTANCE-VALUE receiver (whether its
// static type is spelled directly or through an alias) slip past the check.

namespace {
bool anyDiagMentions(const bezel::test::JitResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}
}  // namespace

// §22.12 anti-regression: a static field accessed through an INSTANCE VALUE
// of the DIRECT (non-alias) type must still be rejected — the companion fix
// touches only the `TypeAliasSymbol` arm and must not weaken this existing
// rejection.
BEZEL_TEST(type_alias_static_member_4038, instance_receiver_direct_type_still_rejected) {
    auto r = runAot(
        "module M; "
        "public class SCInstReject4038 "
        "  public static Counter: int; "
        "  destructor end destructor; "
        "end SCInstReject4038; "
        "class C "
        "  public static method Main(): int "
        "    using var s: SCInstReject4038 := new SCInstReject4038(); "
        "    s.Counter := 5; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(anyDiagMentions(
        r, "must be accessed through the type name, not an instance"));
}

// §22.12 / §27.8 anti-regression: a static field accessed through a LOCAL
// VARIABLE whose DECLARED TYPE is an alias (`var s: AliasOf := new C();
// s.Counter := 5;`) is an INSTANCE-value receiver, not a type-name receiver —
// `s`'s resolved symbol is a LocalSymbol, never the TypeAliasSymbol itself —
// so this must be rejected exactly like the direct-type case above. This is
// the case the companion fix's widened `recvIsType` must NOT newly accept:
// the widening keys on the RECEIVER SYMBOL being a TypeAliasSymbol (the bare
// alias NAME used as receiver), which is a completely different fact from
// the receiver's static TYPE merely being spelled through an alias.
BEZEL_TEST(type_alias_static_member_4038,
           instance_receiver_via_alias_typed_local_still_rejected) {
    auto r = runAot(
        "module M; "
        "type AliasInstReject4038 := SCInstReject4038b; "
        "public class SCInstReject4038b "
        "  public static Counter: int; "
        "  destructor end destructor; "
        "end SCInstReject4038b; "
        "class C "
        "  public static method Main(): int "
        "    using var s: AliasInstReject4038 := new SCInstReject4038b(); "
        "    s.Counter := 5; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(!r.ran);
    BEZEL_EXPECT_TRUE(anyDiagMentions(
        r, "must be accessed through the type name, not an instance"));
}

// ── CROSS-MODULE CELL: alias of an IMPORTED type ───────────────────────────
//
// §2.5 / §27.8: a module-level alias is always module-internal (never part
// of an artifact's public interface, §27.8), but the type it ALIASES may
// perfectly well be imported: `import Lib4038; type A := LibClass4038;`. This
// is the one grid cell the fix's own investigation could not embed in a
// single-source `runAot` test — measured separately with a real two-file
// `bezelc -c` + `--import` compile before being ported here as a permanent
// regression test, using the same `Driver` + `Dependency` two-module harness
// pattern as `xmod_bare_name_pattern_over_imported_2845_test.cpp`.
//
// The alias's OWN declaration (`type A := LibClass4038;`) is local to the
// APP module; only the type it names is imported. Per the fix's mechanism,
// `Resolver::resolveTopDecl` resolves `A`'s `aliasedType` against `LibClass4038`
// through the ordinary imported-type resolution path (the same one a direct,
// non-alias `LibClass4038.Counter` reference in the app module already used
// before this fix) — so nothing importedness-specific needed to change in
// the fix itself. This test exists to CONFIRM that inference is correct
// rather than assume it: an alias resolving to an imported type is exactly
// the shape prior fixes in this run repeatedly found to be "fixed on the
// local axis only."
namespace {

const std::filesystem::path& wtDir4038() {
    return wtScratchDir("wt-4038");
}

struct TempFile4038 {
    std::string path;
    explicit TempFile4038(const std::string& p) : path(p) {}
    ~TempFile4038() { std::remove(path.c_str()); }
};

void writeSource4038(const std::string& path, const std::string& src) {
    std::ofstream out(path);
    out << src;
}

std::pair<int, std::string> runCapture4038(const std::string& exe) {
    std::string out;
    FILE* p = popen((exe + " 2>&1").c_str(), "r");
    if (!p) return {-1, out};
    char buf[256];
    while (std::size_t n = std::fread(buf, 1, sizeof(buf), p)) out.append(buf, n);
    int status = pclose(p);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    return {code, out};
}

}  // namespace

BEZEL_TEST(type_alias_static_member_4038, imported_type_alias_static_field_read_write) {
    auto dir = wtDir4038();
    std::string stem = unique_stem("bezel_4038xmod_");
    TempFile4038 libSrc((dir / (stem + "_lib.bzl")).string());
    TempFile4038 libObj((dir / (stem + "_lib.o")).string());
    TempFile4038 appSrc((dir / (stem + "_app.bzl")).string());
    TempFile4038 appExe((dir / (stem + "_app")).string());

    writeSource4038(libSrc.path,
        "module Lib4038; "
        "public class LibClass4038 "
        "  public static Counter: int; "
        "  public const K: int := 100; "
        "end LibClass4038; ");

    bezel::Driver libDriver;
    bezel::CompileResult libR = libDriver.compileObject(libSrc.path, libObj.path);
    BEZEL_EXPECT_TRUE(libR.success);
    if (!libR.success) return;

    writeSource4038(appSrc.path,
        "module App4038; "
        "import Lib4038; "
        "type AliasImported4038 := LibClass4038; "
        "public class MainClass "
        "  public static method Main(): int "
        "    AliasImported4038.Counter := 63; "
        "    return AliasImported4038.Counter + AliasImported4038.K; "
        "  end Main; "
        "end MainClass; ");

    bezel::Driver appDriver;
    std::vector<bezel::Dependency> deps;
    bezel::Dependency d;
    d.metadataPath = libObj.path;
    d.objectPath = libObj.path;
    deps.push_back(d);
    bezel::CompileResult appR =
        appDriver.compileFile(appSrc.path, appExe.path, deps);
    BEZEL_EXPECT_TRUE(appR.success);
    if (!appR.success) return;

    auto [code, out] = runCapture4038(appExe.path);
    // 63 (written Counter) + 100 (K) == 163 — the VALUE oracle: a
    // compiles-clean-but-wrong-value regression (the #4038 failure shape)
    // would still reach this assertion and be caught by it.
    BEZEL_EXPECT_EQ(code, 163);
}
