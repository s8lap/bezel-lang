// Spec-derived regression tests for issue #6349 — the body of an indexer
// OVERRIDE that a non-generic class declares over a GENERIC base was lowered
// against the base's ERASED layout.
//
// WHAT §23.4, §14.1 AND §12.1 SAY. §23.4 (v0.338): "Indexers may be
// overloaded by parameter type list. They participate in `virtual`/`override`/
// `sealed`/`abstract` like methods." §14.1 makes `E`'s `B<Array<int>>` base
// clause an INSTANTIATION of `B<T>` with `Array<int>` bound for `T`, and §2.8.3
// rule 4 gives that instantiation its own runtime identity. §12.1 makes
// `override property [i: int]: Array<int>` the matching member for
// `B<Array<int>>`'s `[i: int]: T`. So the override accessor is emitted ONCE, for
// `E`, and its body must be lowered under exactly the substitution `E`'s own
// base clause pins.
//
// THE DEFECT (the PRODUCER half of §23.4 generic-indexer dispatch).
// `ensureIndexerGetterFn` / `ensureIndexerSetterFn` (LlvmBackend_Assign.cpp) are
// the emitters that build the function going INTO the vtable slot. They saved
// and restored `genericSubst_` around the body but never INSTALLED one, so
// `lowerExpr` on an accessor body that reads an inherited `T`-typed field GEP'd
// `%class.B` — the erased struct — and loaded the i32 erasure placeholder,
// while the accessor is declared with the CONCRETE substituted element.
//
// MEASURED on this fix's parent (staging `61b280cc`), on the issue's own repro
// with `BEZEL_DUMP_IR=1`:
//
//   define internal ptr @"bz$indexer$get.2"(ptr %0, i32 %1) {
//   entry:
//     %3 = getelementptr inbounds nuw %class.B, ptr %0, i32 0, i32 2
//     %4 = load i32, ptr %3, align 4        ; the slot is ptr at T := Array<int>
//     ret i32 %4
//   }
//   Function return type does not match operand type of return inst!
//   bezelc: codegen error: IR verification failed
//
// THE SIGNATURE WAS NEVER THE PROBLEM. It is built from `lowerType(pd->type)`,
// the OVERRIDE's own concrete spelling (`Array<int>` → `ptr`), which is why the
// verifier aborts: `ptr` declared, `i32` returned. #6319 already fixed the
// CONSUMER half (the three `lowerIndexerGet`/`lowerIndexerSet`/
// `lowerIndexerCompoundCore` dispatch sites that build the signature used to
// CALL the accessor) and #6353 the cross-module wrapper; neither installs a
// substitution for the body built here. The two are disjoint halves and
// `s12_1_0057` exercises only the consumer — its override body is the literal
// `=> "override"` and reads no inherited field, which is why that row went
// green at `73c9a855d8` (#6319) while this defect survived it. Row 6 pins that
// shape as the control.
//
// THE FIX. `accessorAncestorGenericSubst` (LlvmBackend_Assign.cpp) derives the
// substitution a class's OWN base clause induces on its generic ancestors, using
// the SAME `computeAncestorGenericSubst` walk — with a synthesized bare
// receiver, the way `instanceStorageTypes` Phase 1 already does it — and
// `ensureIndexerGetterFn`/`-SetterFn` install it around the BODY emission only.
// The signature computation is untouched.
//
// It REPLACES the ambient substitution rather than merging into it; row 11 pins
// that, and the definition's comment records the merge that was tried, measured
// and reverted.
//
// THE GATES, and what is actually proven about them. ONE gate survives:
// `ps->parent == cls` — the emitters MEMOISE on `(ps, specTag)`, a key that
// partitions by PropertySymbol. An accessor the class declares ITSELF gets one
// function per class, so baking that class's base-clause substitution in is
// sound; an accessor INHERITED unchanged is one shared PropertySymbol that EVERY
// instantiating class reaches under the same key. Under mutation this gate is
// INVISIBLE (no probe program changes behaviour), but what it guards is the
// SILENT #6377 shared-memo-entry class rather than a loud one, so it is kept as
// a judgement, not on evidence.
//
// A SECOND gate — refusing every generic `cls` — was in an earlier version of
// this fix and has been REMOVED. It was mine; the evidence for it did not
// survive measurement. It changed exactly the programs that compute the RIGHT
// answer (row 10 went from a verifier abort to 3), and no probe distinguished
// it from no gate at all: the two-instantiation shape it was meant to guard
// segfaults identically with the gate on and off, and at base.
//
// An unresolvable chain still leaves both maps untouched
// (`computeAncestorGenericSubst` refuses rather than guessing) — the unchanged
// "never install a substitution we cannot prove" discipline.
//
// RESIDUAL, deliberately NOT fixed here, and MEASURED at both ends rather
// than assumed (the earlier version of this file claimed both were "loud
// aborts, the safe direction"; a non-author review REFUTED that and was right):
//   * an accessor INHERITED UNCHANGED by two classes that instantiate `B`
//     differently (`E1: B<Array<int>` with 3, `E2: B<Array<int>` with 9) — NOT
//     loud at all. It compiles, links, runs, exits 0, and SILENTLY PRINTS WRONG
//     VALUES (`0` and `0`): one shared i32-returning memo entry sits in three
//     vtables and is consumed where a pointer is expected. With `string` in the
//     second class it SEGFAULTS instead. Pre-existing, unchanged by this commit,
//     and filed as #6377. Do not read any "safe direction" framing as covering
//     this one.
//   * a two-instantiation generic leaf (`E<Array<int>>` and `E<string>`) prints
//     plausible values and then aborts `free(): invalid pointer`, at base and at
//     tip. Pre-existing; a third shape, not previously reported.
//
// The generic LEAF (row 10) and the generic INTERMEDIATE (rows 8, 9) are all
// FIXED by this commit. An earlier version of this file's prose named "a
// generic intermediate" as an unfixed residual while its own code example was a
// generic leaf; the two are different shapes and the prose conflated them.
//
// HARNESS. `runAotChecked` — the strict AOT oracle (fork-isolated
// `Driver::compileFile`, clang link, native exec under generated-code ASan +
// LSan). Every row asserts BOTH `r.ran` and the value: a compile that aborts at
// the LLVM verifier leaves `ran == false`, and a `didNotCrash`-only row would
// pass on a program that computed the wrong answer. The pre-fix verdict for
// every core row is `ran == false` — the verifier abort, never a wrong value.
//
// Spec: §12.1 (virtual/override matching), §14.1 (generic declarations),
// §23.4 (indexers), §2.8.3 rule 4 (runtime identity per instantiation),
// §27.8 (type aliases, transparent for the element spelling in row 3).
// Revision v0.338.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Both halves are asserted and neither absorbs a toolchain skip: a skip makes
// the harness return `ran = false`, which `BEZEL_EXPECT_TRUE` turns into a row
// failure rather than a silent pass.
void expectRanWith(const std::string& src, int expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

void expectRanStdout(const std::string& src, const std::string& expected) {
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.stdoutText : std::string("<did not run>"),
                    expected);
}

}  // namespace

// §12.1 / §23.4 / §14.1 (#6349) — THE ISSUE'S REPRO, verbatim. The override's
// body reads the inherited `T`-typed field `_a`, and the receiver is DERIVED
// (`var e := new E()`), which is what keeps it off the base-typed dispatch sites
// #6319 repaired. `e[0].Count` is 3 because `B`'s constructor was handed
// `new Array<int>(3, 7)` — the `7` is a fill value, the `3` is the count.
//
// Pre-fix: `ran == false` ("Function return type does not match operand type of
// return inst!" → `codegen error: IR verification failed`).
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           derived_receiver_expr_getter_reads_inherited_field_spec_12_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class E: B<Array<int>> "
        "  public constructor() base(new Array<int>(3, 7)); end constructor; "
        "  public override property [i: int]: Array<int> => _a; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    return e[0].Count; "
        "  end Main; "
        "end Program;",
        3);
}

// §12.1 / §23.4 / §14.1 (#6349) — the same defect reached through a BASE-TYPED
// receiver. The issue records this spelling as the one that reported the
// *consumer* error (`call i32 %idxget`) before #6319 and the *producer* error
// after it, purely because the verifier reports whichever mismatch it reaches
// first. Both were wrong; #6319 took the first, this row is the second.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           base_typed_receiver_expr_getter_reads_inherited_field_spec_12_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class E: B<Array<int>> "
        "  public constructor() base(new Array<int>(3, 7)); end constructor; "
        "  public override property [i: int]: Array<int> => _a; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b: B<Array<int>> := new E(); "
        "    return b[0].Count; "
        "  end Main; "
        "end Program;",
        3);
}

// §12.1 / §27.8 / §23.4 (#6349) — the element spelled through a TYPE ALIAS.
// §27.8 makes an alias transparent for the types it stands for, so `IntList`
// (`:= Array<int>`) in the base clause and in the override's element type is the
// same instantiation as row 1 spelled directly. This is the cell where an
// alias-blind resolution would install nothing: the substitution is keyed by
// the base clause's own argument, which here is the alias head, not `T`.
//
// The constructor argument is spelled `new Array<int>(...)` rather than
// `new IntList(...)` because an alias in a base-constructor argument is a
// SEPARATE, already-tracked defect (`lowerExpr returned null for
// base-constructor argument` — #1386), and mixing the two would make this row
// fail for the wrong reason.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           alias_spelled_element_reads_inherited_field_spec_27_8) {
    expectRanWith(
        "module M; "
        "type IntList := Array<int>; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class E: B<IntList> "
        "  public constructor() base(new Array<int>(4, 9)); end constructor; "
        "  public override property [i: int]: IntList => _a; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    return e[0].Count; "
        "  end Main; "
        "end Program;",
        4);
}

// §12.1 / §23.4 (#6349) — the BLOCK-FORM getter, the spelling that
// `ensureIndexerGetterFn` delegates to `ensureIndexerBlockGetter`. It builds its
// own signature from `lowerType(pd->type)` (already concrete) and lowers its
// body under the ACTIVE substitution — so the install has to be in place around
// the DELEGATION, not only around `lowerExpr` on the expression-bodied path.
// `get => _a;` is the accessor form; `5` is the count handed to the ctor.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           block_getter_reads_inherited_field_spec_12_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T "
        "    get => _a; "
        "  end; "
        "end B; "
        "public class E: B<Array<int>> "
        "  public constructor() base(new Array<int>(5, 2)); end constructor; "
        "  public override property [i: int]: Array<int> "
        "    get => _a; "
        "  end; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    return e[0].Count; "
        "  end Main; "
        "end Program;",
        5);
}

// §12.1 / §23.4 / §28.9 (#6349) — the SETTER twin. `set => _a := value;`
// stores into the inherited `T`-typed field, and the read-back in `Main` goes
// through the same accessor pair. `6` is the count of the array the setter
// stored, which is what makes the row a value assertion rather than a crash
// assertion: had the store gone to the erased slot, the read-back would not
// return 6.
//
// Pre-fix this aborts at the verifier from the paired GETTER's `ret i32 %4`;
// with the getter fixed and the setter not, it aborts from the getter reading a
// slot the setter never wrote. Either way `ran == false`.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           setter_writes_inherited_field_spec_12_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T "
        "    get => _a; "
        "    set => _a := value; "
        "  end; "
        "end B; "
        "public class E: B<Array<int>> "
        "  public constructor() base(new Array<int>(1, 0)); end constructor; "
        "  public override property [i: int]: Array<int> "
        "    get => _a; "
        "    set => _a := value; "
        "  end; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    e[0] := new Array<int>(6, 1); "
        "    return e[0].Count; "
        "  end Main; "
        "end Program;",
        6);
}

// §12.1 / §23.4 — THE CONTROL, and the exact shape of conformance row
// `s12_1_0057` ("an indexer override with a concrete element type dispatches
// through a generic base", §12.1/§14.1). Its override body is the literal
// `=> "override"`: it reads NO inherited field, so the erased GEP this fix
// repairs is simply not on its path. That is why `s12_1_0057` went green at
// `73c9a855d8` (#6319, the consumer half) while this defect survived — the two
// rows exercise DISJOINT code, and neither one licenses the other.
//
// This row is here to catch the fix OVER-reaching: if the install ever fired for
// a body that needs no substitution, or fired for a class whose base clause
// pins nothing, the emitter's state would differ on a program that was already
// correct. Green before, green after, and asserted on the VALUE (stdout `override`
// is the spec-stated observable) rather than on non-crashing.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           constant_body_override_is_unaffected_spec_12_1) {
    expectRanStdout(
        "module M; "
        "public class B<T> "
        "  public constructor() end constructor; "
        "  public virtual property [i: int]: T => default(T); "
        "end B; "
        "public class E: B<string> "
        "  public constructor() end constructor; "
        "  public override property [i: int]: string => \"override\"; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var b: B<string> := new E(); "
        "    Console.WriteLine(b[1]); "
        "    return 0; "
        "  end Main; "
        "end Program;",
        "override\n");
}

// §12.1 / §23.4 — the second over-reach control: a NON-GENERIC base. There is
// no generic ancestor to walk, so
// `accessorAncestorGenericSubst` returns an empty map and both maps are left
// exactly as they were. The override body still reads an
// inherited field, so this is the nearest neighbour of row 1 that must NOT be
// routed through any new path — and `2` is the count handed to the ctor, so a
// silently-wrong read would not pass it.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           non_generic_base_override_is_unaffected_spec_12_1) {
    expectRanWith(
        "module M; "
        "public class B "
        "  private owned _a: Array<int>; "
        "  public constructor() _a := new Array<int>(2, 5); end constructor; "
        "  public virtual property [i: int]: Array<int> => _a; "
        "end B; "
        "public class E: B "
        "  public constructor() base(); end constructor; "
        "  public override property [i: int]: Array<int> => _a; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    return e[0].Count; "
        "  end Main; "
        "end Program;",
        2);
}
// §14.1 / §23.4 / §12.1 (#6349) — a GENERIC INTERMEDIATE. `Mid<U>: B<Array<int>>`
// binds `B`'s `T` to a concrete argument at the INTERMEDIATE, and the
// non-generic `E: Mid<int>` declares the override. The walk is transitive, so
// the substitution reaches `B` through two levels.
//
// This row exists because the RESIDUAL paragraph of the first version of this
// commit named "a generic intermediate" as an unfixed shape while its own code
// example was a generic LEAF — and a non-author review measured that the leaf
// and the intermediate behave differently: the leaf is refused by the
// `cls`-itself-generic gate and still aborts at the verifier, while the
// intermediate is FIXED. Rows 8 and 9 pin the half that is fixed, so the
// residual claim is left describing only what it actually still covers.
//
// MEASURED at this fix's parent (9ffa479b): `Function return type does not
// match operand type of return inst! / ret i32 %4` → `codegen error: IR
// verification failed`. Post-fix: 6.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           generic_intermediate_concrete_base_reads_inherited_field_spec_14_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class Mid<U>: B<Array<int>> "
        "  public constructor() base(new Array<int>(6, 1)); end constructor; "
        "end Mid; "
        "public class E: Mid<int> "
        "  public constructor() base(); end constructor; "
        "  public override property [i: int]: Array<int> => _a; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    return e[0].Count; "
        "  end Main; "
        "end Program;",
        6);
}

// §14.1 / §23.4 / §12.1 (#6349) — the generic intermediate whose base clause
// is written in terms of ITS OWN parameter (`Mid<U>: B<U>`), instantiated
// concretely by the non-generic `E: Mid<Array<int>>`.
//
// This is the level where the substitution genuinely cannot be pinned from
// `Mid` alone — `U` is open there — and where the `computeAncestorGenericSubst`
// refusal discipline is load-bearing: the walk resolves each level through the
// level below, so it must carry `E`'s concrete `Mid<Array<int>>` binding down to
// `B`. `8` is the count handed to the constructor.
//
// MEASURED at this fix's parent (9ffa479b): the same verifier abort. Post-fix: 8.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           generic_intermediate_param_base_reads_inherited_field_spec_14_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class Mid<U>: B<U> "
        "  public constructor(a: owned U) base(move a); end constructor; "
        "end Mid; "
        "public class E: Mid<Array<int>> "
        "  public constructor() base(new Array<int>(8, 2)); end constructor; "
        "  public override property [i: int]: Array<int> => _a; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E(); "
        "    return e[0].Count; "
        "  end Main; "
        "end Program;",
        8);
}

// §14.1 / §23.4 / §12.1 (#6349) — a GENERIC LEAF that declares the override:
// `class E<U>: B<Array<int>>`. `U` is never bound and never mentioned by the
// base clause, so the substitution the body needs (`T := Array<int>`) is fully
// determined by `E`'s own base clause even though `E` is itself generic.
//
// This row exists because an earlier version of this fix carried a second gate
// that refused every generic `cls` outright, on the theory that one memo entry
// is shared across a generic class's instantiations. That gate was DROPPED, and
// this row is why: it refused exactly the programs that compute the RIGHT
// answer (this one went from a verifier abort to 3), while building no probe on
// which it protected anything — the two-instantiation shape it was meant to
// guard segfaults identically with the gate on and off, and at base. The gate
// was mine, added by me, and the evidence for it did not survive measurement.
//
// MEASURED at this fix's parent (27f35b9b): `Function return type does not match
// operand type of return inst! / ret i32 %4`. Post-fix: 3.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           generic_leaf_declares_override_spec_14_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class E<U>: B<Array<int>> "
        "  public constructor() base(new Array<int>(3, 7)); end constructor; "
        "  public override property [i: int]: Array<int> => _a; "
        "end E; "
        "public class Program "
        "  public static method Main(): int "
        "    using var e := new E<string>(); "
        "    return e[0].Count; "
        "  end Main; "
        "end Program;",
        3);
}

// §14.1 / §23.4 / §12.1 (#6349) — an AMBIENT, FOREIGN, SAME-NAMED binding. `G<T>`
// is instantiated `T := int` and lexically encloses the `new E()`, while `E`'s
// own base clause binds a DIFFERENT `T` — `B`'s — to `Array<int>`. The two
// bindings collide by name and nothing in the source relates them.
//
// This row pins that the accessor install REPLACES the ambient substitution
// rather than merging into it. An earlier version of this fix installed with a
// merge that let the ambient binding win the collision, citing
// `ancestorGenericSubstAdditions` (LlvmBackend_Layout.cpp:1793) as precedent;
// that sibling's ambient-wins rule is stated over `t`'s OWN parameters and does
// not generalise to a foreign same-named variable. Under the merge this program
// aborted at the verifier — the body lowered with `T := int` while its
// signature said `Array<int>`. The control that isolates the variable is the
// same program with the ambient parameter RENAMED to `V`, which returns 3 under
// either policy; this row is the colliding spelling.
//
// MEASURED under the merge: verifier abort. Under REPLACE: 3.
BEZEL_TEST(codegen_indexer_override_body_subst_6349,
           foreign_ambient_same_named_binding_is_replaced_spec_14_1) {
    expectRanWith(
        "module M; "
        "public class B<T> "
        "  private owned _a: T; "
        "  public constructor(a: owned T) _a := a; end constructor; "
        "  public virtual property [i: int]: T => _a; "
        "end B; "
        "public class E: B<Array<int>> "
        "  public constructor() base(new Array<int>(3, 7)); end constructor; "
        "  public override property [i: int]: Array<int> => _a; "
        "end E; "
        "public class G<T> "
        "  private _n: T; "
        "  public constructor(n: T) _n := n; end constructor; "
        "  public method Go(): int "
        "    using var e := new E(); "
        "    return e[0].Count; "
        "  end Go; "
        "end G; "
        "public class Program "
        "  public static method Main(): int "
        "    using var g := new G<int>(5); "
        "    return g.Go(); "
        "  end Main; "
        "end Program;",
        3);
}
