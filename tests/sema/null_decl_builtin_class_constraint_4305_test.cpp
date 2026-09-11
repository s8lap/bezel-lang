// fix #4305 (§14.2 "Constraints", stdlib §4.9 `DrainedEntry<K, V>` /
// §2.16 `Shared<T>`/`Weak<T>` / §4.13 owning-enumeration interfaces /
// §6.6 `OwningTaskCompletionSource<T>`, v0.298): a `where` constraint
// declared on a NULL-DECLARATION builtin generic (one of the ~130-name
// population `builtinTypeArity` resolves to a `Named` TypeRef with no
// synthesized `TypeSymbol` — issue #4229) was never checked at all,
// for any of them.
//
// Root cause (verified in this file's own fix, TypeResolution_Names.cpp):
// `Resolver::resolveNamed`'s per-type-argument constraint check —
//
//   if (tsym && !t.genericArgs.empty() &&
//       t.genericArgs.size() == tsym->genericParams.size()) {
//       checkGenericConstraints(t, tsym, ref.genericArgs, scope);
//       ...
//
// — is gated on `tsym` being a real `TypeSymbol`. `DrainedEntry` (like
// `DictionaryEntry`/`IGrouping` before issue #4294's fix) gets no
// synthesized `TypeSymbol`, so `checkGenericConstraints` — the only call
// site that would ever verify a declared `where` clause against a
// supplied argument — never runs for it, or for any other
// null-declaration builtin. This is a DIFFERENT question from issue
// #4229/#4245/#4294/#4309's "is this substitution *defaultable* /
// *constructible*" family (§17.11's `where T: new()` route): this file
// is about whether a supplied type argument satisfies the type's OWN
// declared `where` clause (§14.2), checked at every generic-instantiation
// site, not only at a `default(...)`/`new()` use.
//
// §14.2 quoted in full for the constraint this file pins (v0.298):
//
//   "where T: class | Non-nullable reference types only — classes
//   (including Array<T>, OwningArray<T>, and other heap collection
//   types), interfaces, string, function types."
//
// Quoted stdlib declarations (v0.298):
//
//   stdlib §4.9: "public class DrainedEntry<K, V>: IDisposable
//   where V: class"
//   stdlib §2.16: "public class Shared<T>: IDisposable where T: class"
//                 "public class Weak<T>: IDisposable where T: class"
//   stdlib §4.13: "public interface IOwningEnumerable<T>: IDisposable
//   where T: class" (and its sibling IOwningEnumerator/
//   IOwningAsyncEnumerable/IOwningAsyncEnumerator, same constraint)
//   stdlib §6.6: "public class OwningTaskCompletionSource<T>
//   where T: class"
//
// MEASURED on this fix's parent (`d293926`, this branch's tip before the
// fix, `bezelc` rebuilt directly, `--version` confirmed no `-modified`
// suffix): every REJECT row below compiled clean (exit 0) with NO §14.2
// diagnostic, both via `default(...)` and via a direct type annotation.
// The issue's own repro (`default(DrainedEntry<int, BoxKD>)`) additionally
// RAN clean (exit 0) — a fully silent accept, not merely a compile-time
// gap.
//
// SCOPING NOTE (§25 discipline — this table is NOT the #4229/#4245/#4294
// list, deliberately): `OwningArray`/`OwningSet`/`OwningStack`/
// `OwningQueue`/`OwningLinkedList` (element slot 0) and
// `OwningDictionary` (value slot 1) ALSO carry a `where T: class` /
// `where V: class` clause in their stdlib declarations, but are NOT
// included in this fix or this file: they are already covered by a
// SEPARATE, pre-existing, purpose-built check —
// `Resolver::owningElementBadDesc`'s "owning collection element must be
// class/interface" diagnostic (stdlib §4.2/§14.4, #1327), fired a few
// lines above this fix's own insertion point in
// `Resolver::resolveNamed`. Measured: `default(OwningArray<SomeRecord>)`
// and `default(OwningDictionary<int, SomeRecord>)` ALREADY reject on the
// unpatched parent, through that unrelated mechanism — rows (11)/(12)
// below pin this as a regression control, not a gap this fix closes.
// Folding them into this fix's table would duplicate an existing,
// correct diagnostic for the identical condition.
//
// Atomic<T>/AtomicInteger<T> (`where T: value`), Channel<T>/
// ChannelSender<T> (`where T: sendable`), and SortedDictionary<K,V>/
// SortedSet<T> (`where K/T: IComparable<K/T>`, a self-referential type
// bound) are a DIFFERENT constraint kind each, not `class`, and are
// deliberately out of scope for this fix's table (measured separately,
// still open) — filed as issue #4318.
//
// ── SEND-BACK ADDENDUM (rows 16-24) ─────────────────────────────────────
//
// A review of the original fix (commit 9a9ba8e) found two gaps, both
// against the fix's OWN stated inclusion rule and both fixed in the same
// commit that added these rows:
//
// (1) `IOwningCollection<T>` and `IOwningList<T>` (stdlib §4.2, arity 1,
//     slot 0) qualify unambiguously for the table above — a plain
//     `where T: class` clause, null-declaration builtin, not covered by
//     `owningElementBadDesc` (which fires only for the six named
//     Owning{Array,Set,Stack,Queue,LinkedList,Dictionary} CLASSES, not
//     these two INTERFACES) — and were simply missing. Spec §14.4's
//     "Class-only restriction on owning generics" paragraph (v0.298) names
//     them in the SAME sentence as their four already-fixed siblings
//     (`IOwningEnumerable<T>` etc.): "Most standard-library owning types
//     (`OwningArray<T>`, `OwningLinkedList<T>`, `OwningSet<T>`,
//     `OwningStack<T>`, `OwningQueue<T>`, `OwningDictionary<K, V>`,
//     `OwningTask<T>`, `IOwningEnumerable<T>`, `IOwningEnumerator<T>`,
//     `IOwningAsyncEnumerable<T>`, `IOwningAsyncEnumerator<T>`,
//     `IOwningCollection<T>`, `IOwningList<T>`) restrict their element
//     type T to **class and interface types**." Rows (16)-(19) below.
//
// (2) The original fix's `checkOneConstraint(kind="class")` reuse
//     under-enforces §14.4's STRICTER rule relative to the general
//     `where T: class` (§14.2): §14.4's same paragraph continues "...the
//     standard-library owning types are compiler-enforced to reject the
//     runtime-managed reference types (`string`, function values)" —
//     `checkOneConstraint`'s `"class"` arm calls `categorize()`, whose
//     Primitive/Function arms classify `string`/`Function` as `Reference`
//     (not `Value`), so it never rejected them. MEASURED on the original
//     fix's own binary (9a9ba8e, `bezelc` rebuilt, `--version` confirmed):
//     `default(IOwningEnumerable<string>)` compiled clean (exit 0).
//     Fixed by reusing `owningElementBadDesc` (TypeResolution_Checks.cpp,
//     held this cycle, read but not modified) directly — the SAME
//     authority the `ownedSlot` check a few lines above already uses for
//     the six Owning-collection classes, and §14.4 names this table's ten
//     names in the identical sentence, so it is provably the same
//     question (§6/§25), not a predicate reused on a hunch.
//
//     That swap ALSO fixed a THIRD, previously-unflagged defect with the
//     identical root cause: `checkOneConstraint`'s `"class"` arm
//     unconditionally rejects `arg->nullable`, but the very next spec
//     sentence after the one quoted above states the opposite for this
//     exact family — "Nullable class types and interfaces are admitted.
//     The owning-type instantiation rule checks the *underlying* type, not
//     the nullable-wrapper" (with `OwningTask<Lock?>` as its own worked
//     example). MEASURED on the original fix's binary: `Shared<SomeClass?>`
//     was WRONGLY REJECTED ("does not satisfy the constraint `where T:
//     class`") despite being spec-legal. `owningElementBadDesc` never
//     inspects `->nullable` at all, so reusing it fixes this for free —
//     row (23) pins it. This is not scope creep: it is an unavoidable
//     consequence of mirroring the correct authority rather than the
//     narrower one, on the SAME `checkOneConstraint`-vs-`owningElementBadDesc`
//     axis as the string/function fix.
//
// The candidate-set sweep for (1) was MECHANICAL: every arity>=1 name in
// `Resolver::builtinTypeArity` (TypeResolution_Names.cpp) was cross-
// referenced against every TYPE-LEVEL (not method-level) `where <slot>:
// ...` declaration clause in docs/Bezel_Standard_Library.md. `IOwningCollection`/
// `IOwningList` were the ONLY names the sweep added; see the fix commit's
// TypeResolution_Names.cpp comment for the full accounting of every other
// candidate and why it is excluded (owningElementBadDesc-covered, a
// different constraint kind covered by issue #4318, a real synthesized
// TypeSymbol so never null-declaration, or a method-level constraint on an
// unrelated static factory).

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation, diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

namespace {

// Asserts a clean, real-location §14.2 rejection naming the `class`
// constraint on the given parameter name.
void expectClassConstraintRejected(const std::string& program,
                                    const std::string& paramName) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("does not satisfy the constraint `where " + paramName +
            ": class`")
               .c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""14.2"));
}

}  // namespace

// ── (1) the issue's own repro: top-level `default(DrainedEntry<K,V>)`
//        with a record V — compiled AND ran clean pre-fix ────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           default_operator_rejects_drainedentry_record_v_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305a; "
        "public record BoxKD4305a(N: int); "
        "class Prog4305a "
        "  public static method Main(): int "
        "    var x := default(DrainedEntry<int, BoxKD4305a>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305a;",
        "V");
}

// ── (2) DrainedEntry via a DIRECT type annotation (no `default`) — the
//        exact `resolveNamed`/`checkGenericConstraints` call site this
//        fix patches, independent of the `default` operator's own
//        machinery ─────────────────────────────────────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           direct_annotation_rejects_drainedentry_record_v_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305b; "
        "public record BoxKD4305b(N: int); "
        "class Prog4305b "
        "  public static method Main(): int "
        "    var x: DrainedEntry<int, BoxKD4305b>? := null; "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305b;",
        "V");
}

// ── (3) DrainedEntry as the INNER generic argument of an outer
//        `where T: new()` substitution — the constraint check runs at
//        EVERY generic-instantiation site (§14.2), not only a top-level
//        one; this is `resolveNamed`'s ordinary recursive-argument walk,
//        so it is verified as its own row rather than assumed ─────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           where_T_new_wrapping_rejects_drainedentry_record_v_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305c; "
        "public record BoxKD4305c(N: int); "
        "class HolderDE4305c<T> where T: new() "
        "  public static method Make(): owned T => new T(); "
        "end HolderDE4305c; "
        "class Prog4305c "
        "  public static method Main(): int "
        "    var b := HolderDE4305c<DrainedEntry<int, BoxKD4305c>>.Make(); "
        "    _ := b; "
        "    return 0; "
        "  end Main; "
        "end Prog4305c;",
        "V");
}

// ── (4) Shared<T> via `default(...)` ──────────────────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           default_operator_rejects_shared_record_t_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305d; "
        "public record BoxKD4305d(N: int); "
        "class Prog4305d "
        "  public static method Main(): int "
        "    var x := default(Shared<BoxKD4305d>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305d;",
        "T");
}

// ── (5) Weak<T> via `default(...)` ─────────────────────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           default_operator_rejects_weak_record_t_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305e; "
        "public record BoxKD4305e(N: int); "
        "class Prog4305e "
        "  public static method Main(): int "
        "    var x := default(Weak<BoxKD4305e>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305e;",
        "T");
}

// ── (6) IOwningEnumerable<T> via a direct annotation ──────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           direct_annotation_rejects_iowningenumerable_record_t_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305f; "
        "public record BoxKD4305f(N: int); "
        "class Prog4305f "
        "  public static method Main(): int "
        "    var x: IOwningEnumerable<BoxKD4305f>? := null; "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305f;",
        "T");
}

// ── (7) IOwningEnumerator<T> via a direct annotation ───────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           direct_annotation_rejects_iowningenumerator_record_t_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305g; "
        "public record BoxKD4305g(N: int); "
        "class Prog4305g "
        "  public static method Main(): int "
        "    var x: IOwningEnumerator<BoxKD4305g>? := null; "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305g;",
        "T");
}

// ── (8) IOwningAsyncEnumerable<T> via `default(...)` ───────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           default_operator_rejects_iowningasyncenumerable_record_t_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305h; "
        "public record BoxKD4305h(N: int); "
        "class Prog4305h "
        "  public static method Main(): int "
        "    var x := default(IOwningAsyncEnumerable<BoxKD4305h>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305h;",
        "T");
}

// ── (9) IOwningAsyncEnumerator<T> via `default(...)` ───────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           default_operator_rejects_iowningasyncenumerator_record_t_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305i; "
        "public record BoxKD4305i(N: int); "
        "class Prog4305i "
        "  public static method Main(): int "
        "    var x := default(IOwningAsyncEnumerator<BoxKD4305i>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305i;",
        "T");
}

// ── (10) OwningTaskCompletionSource<T> via `default(...)` ──────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           default_operator_rejects_owningtaskcompletionsource_record_t_spec_14_2) {
    expectClassConstraintRejected(
        "module M4305j; "
        "public record BoxKD4305j(N: int); "
        "class Prog4305j "
        "  public static method Main(): int "
        "    var x := default(OwningTaskCompletionSource<BoxKD4305j>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305j;",
        "T");
}

// ── (11) REGRESSION-PIN CONTROL: OwningArray<T> with a record T is
//         DELIBERATELY excluded from this fix's table (see the file
//         header) — it must go on rejecting through its PRE-EXISTING
//         `owningElementBadDesc` diagnostic, unchanged by this fix ─────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           owningarray_record_t_still_rejects_via_preexisting_check_control) {
    auto r = runAot(
        "module M4305k; "
        "public record BoxKD4305k(N: int); "
        "class Prog4305k "
        "  public static method Main(): int "
        "    var x := default(OwningArray<BoxKD4305k>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305k;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "requires a class or interface element type"));
}

// ── (12) REGRESSION-PIN CONTROL: OwningDictionary<K,V> with a record V,
//         same reasoning as (11), value slot 1 ─────────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           owningdictionary_record_v_still_rejects_via_preexisting_check_control) {
    auto r = runAot(
        "module M4305l; "
        "public record BoxKD4305l(N: int); "
        "class Prog4305l "
        "  public static method Main(): int "
        "    var x := default(OwningDictionary<int, BoxKD4305l>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305l;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "requires a class or interface element type"));
}

// ── (13) POSITIVE CONTROL: DrainedEntry<int, ClassT> (a satisfying
//         class substitution) via `default(...)` must stay ACCEPTED —
//         cheap insurance that the new check does not over-reject a
//         legitimate class argument ─────────────────────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           default_operator_accepts_drainedentry_class_v_control) {
    auto r = runAot(
        "module M4305m; "
        "public class ClsA4305m "
        "  public property N: int := 0; "
        "end ClsA4305m; "
        "class Prog4305m "
        "  public static method Main(): int "
        "    var x := default(DrainedEntry<int, ClsA4305m>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305m;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (14) POSITIVE CONTROL, the load-bearing one (LANE_RULES §15): REAL
//         `DrainedEntry<K, V>` usage obtained from the actual collection
//         API — `OwningDictionary<K, V>.Drain()` (stdlib §4.9) — must
//         compile AND run with the CORRECT value. This is the exact
//         production path this fix must not disturb: `Drain()`'s
//         `IOwningEnumerable<DrainedEntry<K, V>>` return type is an
//         INFERRED type, not a written annotation, so it never reaches
//         `resolveNamed`'s `t.genericArgs` walk this fix patches — this
//         row proves that indifference empirically rather than assuming
//         it from the code path. ────────────────────────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           owningdictionary_drain_real_api_accepts_and_sums_correctly_control) {
    auto r = runAot(
        "module M4305n; "
        "public class ClsA4305n "
        "  public property N: int; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end ClsA4305n; "
        "class Prog4305n "
        "  public static method Main(): int "
        "    using var d := new OwningDictionary<int, ClsA4305n>(); "
        "    d.Add(1, new ClsA4305n(10)); "
        "    d.Add(2, new ClsA4305n(20)); "
        "    var total := 0; "
        "    for var entry in d.Drain() do "
        "      total := total + entry.Value.N; "
        "      entry.Dispose(); "
        "    end for; "
        "    return total; "
        "  end Main; "
        "end Prog4305n;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 30);
}

// ── (15) POSITIVE CONTROL: real `Shared<T>`/`Weak<T>` construction and
//         use at a satisfying class T must be unaffected ───────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           shared_and_weak_real_class_t_usage_control) {
    auto r = runAot(
        "module M4305o; "
        "public class ClsA4305o "
        "  public property N: int; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end ClsA4305o; "
        "class Prog4305o "
        "  public static method Main(): int "
        "    using var sh := new Shared<ClsA4305o>(new ClsA4305o(42)); "
        "    var got := sh.Get().N; "
        "    using var wk := sh.Downgrade(); "
        "    return got; "
        "  end Main; "
        "end Prog4305o;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// ── (16) FINDING 1: IOwningCollection<T> via `default(...)` — the name the
//         original fix's own inclusion rule missed ─────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           default_operator_rejects_iowningcollection_record_t_spec_14_4) {
    expectClassConstraintRejected(
        "module M4305p; "
        "public record BoxKD4305p(N: int); "
        "class Prog4305p "
        "  public static method Main(): int "
        "    var x := default(IOwningCollection<BoxKD4305p>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305p;",
        "T");
}

// ── (17) FINDING 1: IOwningList<T> via a direct annotation ─────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           direct_annotation_rejects_iowninglist_record_t_spec_14_4) {
    expectClassConstraintRejected(
        "module M4305q; "
        "public record BoxKD4305q(N: int); "
        "class Prog4305q "
        "  public static method Main(): int "
        "    var x: IOwningList<BoxKD4305q>? := null; "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305q;",
        "T");
}

// ── (18) FINDING 1, POSITIVE CONTROL, the load-bearing one (LANE_RULES
//         §15): a REAL `IOwningCollection<T>` usage — `OwningArray<T>`
//         (stdlib §4.2's own IOwningList<T>/IOwningCollection<T> conformer)
//         moved into an `IOwningCollection<Widget>`-typed local — must
//         still compile and its `Dispose()` must still cascade through the
//         held element. Mirrors the proven shape of
//         tests/codegen/interface_head_dispose_3026_test.cpp's
//         `owningarray_iowningcollection_cascades_disposal_28_9`. ────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           iowningcollection_real_usage_cascades_disposal_control) {
    auto r = runAot(
        "module M4305r; "
        "class Widget4305r "
        "  public static Dtors: int := 0; "
        "  destructor Widget4305r.Dtors := Widget4305r.Dtors + 1; "
        "  end destructor; "
        "end Widget4305r; "
        "class Prog4305r "
        "  public static method Main(): int "
        "    var arr := new OwningArray<Widget4305r>(1); "
        "    arr.Add(new Widget4305r()); "
        "    var e: IOwningCollection<Widget4305r> := move arr; "
        "    e.Dispose(); "
        "    if Widget4305r.Dtors <> 1 then "
        "      return 92; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end Prog4305r;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (19) FINDING 1, POSITIVE CONTROL: a REAL `IOwningList<T>` usage —
//         indexer read through the interface after a `move` — must still
//         compile, run, and produce the correct value ──────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           iowninglist_real_usage_indexer_accepts_and_returns_correctly_control) {
    auto r = runAot(
        "module M4305s; "
        "class Widget4305s "
        "  public property N: int; "
        "  public constructor(n: int) "
        "    N := n; "
        "  end constructor; "
        "end Widget4305s; "
        "class Prog4305s "
        "  public static method Main(): int "
        "    var arr := new OwningArray<Widget4305s>(1); "
        "    arr.Add(new Widget4305s(7)); "
        "    var e: IOwningList<Widget4305s> := move arr; "
        "    var got := e[0].N; "
        "    e.Dispose(); "
        "    return got; "
        "  end Main; "
        "end Prog4305s;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── (20)-(22) FINDING 2: §14.4's STRICTER rule — "the standard-library
//         owning types are compiler-enforced to reject the runtime-managed
//         reference types (`string`, function values)" — was enforced for
//         the six `ownedSlot`-covered collection CLASSES but not for the
//         `checkOneConstraint`-routed names (the four `IOwning*Enumerable/
//         Enumerator` siblings and now IOwningCollection/IOwningList), the
//         same §14.4 sentence enforced in half. MEASURED on the original
//         fix's own binary (9a9ba8e): `default(IOwningEnumerable<string>)`
//         compiled clean (exit 0), with NO diagnostic. ─────────────────
static void expectRuntimeManagedRefRejected(const std::string& program,
                                             const std::string& paramName) {
    auto r = runAot(program);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, ("does not satisfy the constraint `where " + paramName +
            ": class`")
               .c_str()));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""14.2"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "\xC2\xA7""14.4"));
}

BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           iowningenumerable_rejects_string_runtime_managed_ref_spec_14_4) {
    expectRuntimeManagedRefRejected(
        "module M4305t; "
        "class Prog4305t "
        "  public static method Main(): int "
        "    var x := default(IOwningEnumerable<string>); "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305t;",
        "T");
}

BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           iowningenumerator_rejects_string_runtime_managed_ref_spec_14_4) {
    expectRuntimeManagedRefRejected(
        "module M4305u; "
        "class Prog4305u "
        "  public static method Main(): int "
        "    var x: IOwningEnumerator<string>? := null; "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305u;",
        "T");
}

// Completes §14.4's stated PAIR ("string, function values"): a function
// value must be rejected too, not only `string`.
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           iowningenumerable_rejects_function_value_runtime_managed_ref_spec_14_4) {
    expectRuntimeManagedRefRejected(
        "module M4305v; "
        "class Prog4305v "
        "  public static method Main(): int "
        "    var x: IOwningEnumerable<(int) -> int>? := null; "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305v;",
        "T");
}

// ── (23) FINDING 2 SIDE-FIX, REGRESSION PIN: §14.4's very next sentence
//         after the "Class-only restriction" paragraph — "Nullable class
//         types and interfaces are admitted. The owning-type instantiation
//         rule checks the *underlying* type, not the nullable-wrapper" —
//         is the OPPOSITE of `checkOneConstraint`'s blanket `arg->nullable`
//         rejection. MEASURED on the original fix's own binary (9a9ba8e):
//         `Shared<SomeClass?>` was WRONGLY REJECTED ("does not satisfy the
//         constraint `where T: class`") despite being spec-legal —
//         `owningElementBadDesc` never inspects nullability, so reusing it
//         (Finding 2's fix) corrects this as a side effect. This program
//         must ACCEPT. ──────────────────────────────────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           shared_nullable_class_t_admitted_per_spec_14_4_control) {
    auto r = runAot(
        "module M4305w; "
        "public class ClsA4305w "
        "  public property N: int := 0; "
        "end ClsA4305w; "
        "class Prog4305w "
        "  public static method Main(): int "
        "    var x: Shared<ClsA4305w?>? := null; "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305w;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ── (24) FINDING 1 + FINDING 2 TOGETHER: the stricter §14.4 rule applies
//         to a NEWLY-ADDED name too, not only the four names the original
//         fix already routed through `checkOneConstraint` — proves the
//         string/function fix is not accidentally scoped to the original
//         four ────────────────────────────────────────────────────────
BEZEL_TEST(null_decl_builtin_class_constraint_4305,
           iowningcollection_rejects_string_runtime_managed_ref_spec_14_4) {
    expectRuntimeManagedRefRejected(
        "module M4305x; "
        "class Prog4305x "
        "  public static method Main(): int "
        "    var x: IOwningCollection<string>? := null; "
        "    _ := x; "
        "    return 0; "
        "  end Main; "
        "end Prog4305x;",
        "T");
}
