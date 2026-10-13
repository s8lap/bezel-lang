// Spec-derived regression test for issue #3794 — sema did not reject a move-only
// value type T as the element of `OwningBuffer<T>` / `Span<T>` / `ReadOnlySpan<T>`,
// even though spec §14.4's OwningBuffer/Span/ReadOnlySpan exception explicitly
// preserves the §15.3 move-only rejection.
//
// THE GOVERNING RULE. Spec §14.4 (heading "`owned` and generic type arguments"),
// bolded paragraph (v0.286):
//
//   "`OwningBuffer<T>`, `Span<T>`, and `ReadOnlySpan<T>` (stdlib §4.14) are
//    exceptions to this restriction. They admit T to be either class-typed
//    (with ownership semantics as above) or value-typed (with plain value
//    semantics — no per-slot ownership, no Dispose cascade, no `move`
//    admission). They still reject the runtime-managed reference types
//    (`string`, function values) and **move-only value types** — both kinds
//    of §15.3: the owning inline arrays `[N owned]T` and `[N owned]T?`, and
//    the owning-classified choice (§35.4). Value-like and borrow-bearing
//    choices are copyable value types (§15.3) and are admitted as value-typed
//    T, as stdlib §4.14 sets out."
//
// §15.3 (heading "Move-only value types"): "The move-only value type kinds
// in this specification are two: the owning inline array `[N owned]T`/`[N
// owned]T?` and the owning-classified choice (§35.4)." §35.4 rule 1: "A
// choice is owning if any case has a payload field marked `owned` ... or a
// payload field of a move-only value type."
//
// ISSUE'S OWN REPRO (measured on r12-cycle-1 = 012f6f2, this lane's base
// commit, BEFORE this fix):
//
//   module M;
//   class Conn public id: int := 0; end Conn;
//   choice Wrap Item(owned c: Conn), None; end Wrap;
//   class C public static method Main(): int
//     using var b := new OwningBuffer<Wrap>();
//     return 0;
//   end Main; end C;
//
// The PARENT compiler compiled and linked this with ZERO diagnostics. With
// the FIX, it is rejected with the §14.4 / §15.3 diagnostic asserted below.
//
// ROOT CAUSE. The §14.4 OwningBuffer/Span/ReadOnlySpan exception's move-only
// clause was not enforced. `Resolver::checkElementCategoryInstantiations`
// (TypeResolution_Checks.cpp, fix #3775) is the existing §28.11 authority
// for the §28.11 element-slot-policy table's move-only row across the §28.11
// collection families — but #3775's commit message explicitly defers
// OwningBuffer/Span/ReadOnlySpan (different admission rule, different spec
// citation) and files them as a separate issue (#3794). No sema gate
// previously consulted `common::isMoveOnlyValueType` at the OwningBuffer/
// Span/ReadOnlySpan annotation site at all.
//
// THE FIX. A NEW sema gate in `resolveType` (TypeResolution_Names.cpp):
// Pass 1 (concrete-bound T) is INLINE at the annotation site — if
// `common::isMoveOnlyValueType` answers true, emit the §14.4 / §15.3
// diagnostic at the type argument (fix #3796 §3.6 rule 2: the instantiation
// site, not the surrounding annotation). Pass 2 (bare generic parameter of
// the ENCLOSING type, e.g. `field: OwningBuffer<T>` inside `class H<T>`) is
// recorded at the annotation site with the same method-shadowing guard as
// the #2713 owning-element recorder (`class H<T> { method M<T>() }`), and
// re-checked per instantiation of the enclosing type by a new
// `Resolver::checkOwningBufferSpanMoveOnlyInstantiations` (TypeResolution_Checks.cpp),
// modelled on `checkElementCategoryInstantiations`'s Pass 2 and
// `checkOwningElementInstantiations`'s #2713 shape. The diagnostic cites
// §14.4 (not §28.11) because the rule lives in the §14.4 exception clause,
// and names the §15.3 move-only kind the slot's bound type is
// (owning-classified choice / owning inline array).
//
// SCOPE. All three OwningBuffer/Span/ReadOnlySpan types — `OwningBuffer<T>`,
// `Span<T>`, `ReadOnlySpan<T>` — are covered, both for the §15.3 owning-
// classified choice kind AND the §15.3 owning inline array kind. The
// Pass-2-instantiation path is covered. ADMIT controls pin value-typed T
// (an `int` primitive, a class type `Resource`, and a value-like choice)
// and the Pass-2 uninstantiated-template guard.
//
// FAIL-WITHOUT / PASS-WITH (measured against this lane's rebase, 012f6f2):
// the PARENT bezelc (built via the same worktree, with the fix's source
// change reverted) compiles the issue's own repro with ZERO diagnostics;
// the FIX compiler rejects it with the §14.4 / §15.3 diagnostic asserted
// below.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "driver/Driver.hpp"
#include "support/aot_support.hpp"
#include "support/driver_run_support.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace bezel::sema {
namespace {

// True iff an ERROR diagnostic is THIS defect's §14.4 / §15.3 rejection:
// the container name (OwningBuffer / Span / ReadOnlySpan), the §14.4 and
// §15.3 citation, and a move-only-kind substring (`moveOnlyKind`).
// Deliberately NOT a bare "compile failed": `runAot` synthesizes an Error
// diagnostic for a runtime crash too (LANE_RULES §3). Pass-1 (inline) and
// Pass-2 (re-check) wording differ slightly (Pass-1 says "an
// owning-classified choice" / "an owning inline array", Pass-2 names the
// bound type); this checks for the shared substrings both share.
bool hasOwningBufferSpanMoveOnlyError(const JitResult& r,
                                      const std::string& container,
                                      const std::string& moveOnlyKind) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("\xC2\xA7""14.4") == std::string::npos) continue;
        if (d.message.find("\xC2\xA7""15.3") == std::string::npos) continue;
        if (d.message.find("`" + container + "`") == std::string::npos)
            continue;
        if (d.message.find("its T type argument") == std::string::npos)
            continue;
        if (d.message.find(moveOnlyKind) == std::string::npos) continue;
        return true;
    }
    return false;
}

// Compiles, links, runs, and exits with `expected`.
bool runs(int expected, const std::string& src) {
    auto r = runAot(src);
    return r.ran && r.exitCode == expected;
}

// A choice with an `owned` class payload — §35.4 rule 1, owning-classified,
// hence move-only (§15.3). `Conn` is the class payload.
const std::string kOwningChoiceDecl =
    "class Conn public id: int := 0; end Conn; "
    "choice Wrap Item(owned c: Conn), None; end Wrap; ";

// A choice with NO `owned`/class payload — §35.4 rule 1, value-like (not
// move-only), admitted as value-typed T per §14.4's exception.
const std::string kValueLikeChoiceDecl =
    "choice VLC A, B(n: int); end VLC; ";

}  // namespace

// =============================================================================
// REJECT — Pass 1 (concrete slots). Every case asserts diagnostic TEXT.
// =============================================================================

// §14.4 / §15.3 / §35.4 — owning-classified choice as OwningBuffer's T.
// This is the issue's own repro shape.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           owning_buffer_owning_choice_rejected_3794) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningBufferSpanMoveOnlyError(
        r, "OwningBuffer", "an owning-classified choice"));
}

// §14.4 / §15.3 / §35.4 — owning-classified choice as Span's T.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           span_owning_choice_rejected_3794) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<Wrap>(3); "
        "  var sp: Span<Wrap> := b.AsSpan(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningBufferSpanMoveOnlyError(
        r, "Span", "an owning-classified choice"));
}

// §14.4 / §15.3 / §35.4 — owning-classified choice as ReadOnlySpan's T.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           readonly_span_owning_choice_rejected_3794) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<Wrap>(3); "
        "  var ros: ReadOnlySpan<Wrap> := b.AsReadOnlySpan(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningBufferSpanMoveOnlyError(
        r, "ReadOnlySpan", "an owning-classified choice"));
}

// §14.4 / §15.3 — the OTHER move-only kind, owning inline array, as
// OwningBuffer's T. The §15.3 kinds are TWO; the diagnostic must reject
// both, not just the choice kind.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           owning_buffer_inline_array_rejected_3794) {
    auto r = runAot(
        "module M; "
        "class R public x: int := 0; end R; "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<[5 owned]R>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningBufferSpanMoveOnlyError(
        r, "OwningBuffer", "an owning inline array"));
}

// =============================================================================
// REJECT — Pass 2 (bare generic parameter of the ENCLOSING type, re-checked
// per instantiation). Modelled on checkOwningElementInstantiations's
// #2713 shape: the annotation site does not know the concrete T, so it is
// recorded at the annotation site and re-checked when each instantiation of
// the enclosing type binds T to a concrete type.
// =============================================================================

// `class H<T>` holding `field: OwningBuffer<T>`. Instantiated with the
// owning-classified `Wrap` choice — Pass 2 must reject at the `H<Wrap>`
// instantiation site (the type argument's location, per fix #3796 §3.6
// rule 2), with the same §14.4 / §15.3 diagnostic.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           generic_bare_param_bound_to_owning_choice_rejected_3794) {
    auto r = runAot(
        "module M; " + kOwningChoiceDecl +
        "class H<T> where T: class "
        "  private owned f: OwningBuffer<T> := new OwningBuffer<T>(); "
        "  public method N(): int return f.Length; end N; "
        "end H; "
        "class C public static method Main(): int "
        "  using var h := new H<Wrap>(); "
        "  return 0; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(hasOwningBufferSpanMoveOnlyError(
        r, "OwningBuffer", "Wrap"));
}

// =============================================================================
// ACCEPT CONTROLS — a false reject is worse than the bug (LANE_RULES §7).
// =============================================================================

// fix #6081 (stdlib §4.14): the probe bodies below read the buffer through
// `Length` (its declared member); `OwningBuffer<T>` declares no `Count`, so
// the former `.Count` reads are now "no member" rejects. The admission or
// move-only property each row pins is unchanged.
//
// §14.4 OwningBuffer/Span/ReadOnlySpan exception clause: T may be a
// CLASS TYPE (with ownership semantics as above) — `Resource` is a
// non-owned class.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           owning_buffer_class_accepted_runs_3794) {
    BEZEL_EXPECT_TRUE(runs(
        3,
        "module M; "
        "class Resource public x: int := 0; end Resource; "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<Resource>(3); "
        "  return b.Length; "
        "end Main; end C;"));
}

// §14.4 OwningBuffer/Span/ReadOnlySpan exception clause: T may be a
// VALUE TYPE (with plain value semantics — no per-slot ownership, no
// Dispose cascade, no `move` admission) — `int` is a primitive value type.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           owning_buffer_int_accepted_runs_3794) {
    BEZEL_EXPECT_TRUE(runs(
        7,
        "module M; "
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<int>(7); "
        "  return b.Length; "
        "end Main; end C;"));
}

// §14.4: "Value-like and borrow-bearing choices are copyable value types
// (§15.3) and are admitted as value-typed T, as stdlib §4.14 sets out." A
// value-like choice is not move-only (§15.3 lists only the owning inline
// array and the owning-classified choice) and must stay legal. Fix #5821
// (§14.2 two-presentation paragraph, v0.338; stdlib §4.14): no choice type is
// defaultable (§17.11, §35.10), so the one-argument constructor is rejected
// for a choice element; a choice-element buffer is built with the
// two-argument init-function constructor. The row keeps its §14.4 property
// (admitted, not move-only, runs) on that spec-admitted form.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           owning_buffer_valuelike_choice_accepted_runs_3794) {
    BEZEL_EXPECT_TRUE(runs(
        2,
        "module M; " + kValueLikeChoiceDecl +
        "class C public static method Main(): int "
        "  using var b := new OwningBuffer<VLC>(2, lambda(i: int): VLC => VLC.A); "
        "  return b.Length; "
        "end Main; end C;"));
}

// ⚠ THE FALSE-REJECT GUARD for Pass 2, mirroring #2713's
// `uninstantiated_generic_template_admitted_2713`: inside a generic body
// that is never instantiated, T is a placeholder (Kind::Named, declaration
// == nullptr) — common::isMoveOnlyValueType must answer false for it, or
// every uninstantiated `OwningBuffer<T>` field anywhere would falsely
// reject at its own declaration. The body here is `class H<T>` with no
// instantiation in this module; `H` must NOT trip the Pass-2 check.
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           generic_uninstantiated_template_accepted_runs_3794) {
    BEZEL_EXPECT_TRUE(runs(
        5,
        "module M; "
        "class H<T> where T: class "
        "  private owned f: OwningBuffer<T> := new OwningBuffer<T>(); "
        "  public method N(): int return f.Length; end N; "
        "end H; "
        "class C public static method Main(): int return 5; end Main; "
        "end C;"));
}

// The Pass-2 ACCEPT half: a generic parameter bound to a proper CLASS
// through `H<Resource>` must run cleanly (mirrors #3775's
// `generic_bare_param_bound_to_class_accepted_runs_3775`).
BEZEL_TEST(sema_span_owning_buffer_move_only_rejected_3794,
           generic_bare_param_bound_to_class_accepted_runs_3794) {
    BEZEL_EXPECT_TRUE(runs(
        0,
        "module M; "
        "class Resource public x: int := 0; end Resource; "
        "class H<T> where T: class "
        "  private owned f: OwningBuffer<T> := new OwningBuffer<T>(); "
        "  public method N(): int return f.Length; end N; "
        "end H; "
        "class C public static method Main(): int "
        "  using var h := new H<Resource>(); "
        "  return h.N(); "
        "end Main; end C;"));
}

}  // namespace bezel::sema
