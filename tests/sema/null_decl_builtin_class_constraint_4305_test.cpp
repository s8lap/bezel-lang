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
