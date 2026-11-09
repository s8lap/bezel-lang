// Codegen — issues #7887 / #7941 (§28.12 step 1 / step 2, v0.355): a GENERIC
// `await using` head ran BOTH its `DisposeAsync` and its destructor body.
//
// WHAT WAS WRONG. §28.12 step 2's physical reclamation is emitted per CLASS:
// the table `disposerReclaimFns_` holds one `bz$<C>$DisposeReclaim` per class,
// and the population gate creates one only for a class with an EMPTY
// `genericParams` — a generic class's owned-field classification is
// per-instantiation, so there is no single per-class reclaim function to call.
// A generic head therefore had NO entry in that table, and the disposal site's
// DEGRADE arm ran `emitDisposeAndReclaim(obj)` — the vtable's most-derived
// `Dispose`, which runs the DESTRUCTOR BODY. §28.12 step 1 says the
// destructor body is NOT also run, because `DisposeAsync` supersedes it:
// "the destructor body (if any) is not also run, because `DisposeAsync`
// supersedes it — per class, along the reclamation chain (step 2)".
//
// MEASURED on this tree's base (r30-c1), under `bezelc --sanitize=address`
// (ASan + LSan): the issue's own repro printed `oda=100 odt=100` — the
// `DisposeAsync` body ran 100 times AND the destructor body ran 100 times for
// the same 100 objects. The expected output is `oda=100 odt=0`.
//
// THE ROWS. Each row's observable ENCODES THE COUNTS, not merely "no leak": a
// test that only asserted "the program did not leak" would pass on the base
// (the base leaks nothing either — it ran the destructor body INSTEAD of
// skipping it) and would not be a repro of this defect at all.
//
//   * `generic_head_skips_its_destructor_body` — the repro row. The generic
//     head's `DisposeAsync` runs (oda = 1) and its destructor body does NOT
//     (odt = 0), and the owned field is still reclaimed (cells = 0).
//   * `generic_head_per_level_base_destructor_still_runs` — the per-level row
//     of §28.12 step 2: a BASE level that declares no `DisposeAsync` and has
//     a destructor STILL runs its destructor body (bdt = 1) while the derived
//     level's is skipped (odt = 0). A fix that skipped every destructor body
//     would fail this row, and so would one that ran the derived level's.
//   * `concrete_head_is_unchanged` — the CONTROL: the ordinary non-generic
//     head's path is exactly what it was (`oda = 1`, `odt = 0`, `cells = 0`).
//   * `sync_using_var_still_runs_the_destructor_body` — the CONTROL of §28.12
//     step 1's other half: a plain `using var` runs the destructor body
//     synchronously and does NOT call `DisposeAsync` (odt = 1, oda = 0).
//   * `two_instantiations_of_one_generic_get_separate_reclaim_bodies` — THE
//     CACHE-KEY row. Two instantiations of ONE generic head whose type
//     arguments ERASE ALIKE (`Dog` and `Task<int>` both lower to `ptr`) but
//     whose owned-field classification DIFFERS (a class reference is disposed
//     through the vtable; a Task-family value is disposed by the task path)
//     must get SEPARATE per-instantiation reclaim bodies. A cache keyed on the
//     erased field-type vector alone gives them ONE body, and the second
//     instantiation is then reclaimed with the FIRST one's classification —
//     the owned `Dog` is stranded (its destructor body never runs) or the
//     `Task<int>` is disposed as a class reference, which reads through a
//     garbage pointer and aborts under ASan.
//   * `generic_method_monomorphizations_get_separate_reclaim_bodies` — THE
//     SAME ROW, one level harder. The head's type argument is the METHOD's
//     own type parameter (`Box<T>` inside `Run<T>`), so the head's
//     instantiation is not concrete at the binding site and the SYNTACTIC
//     argument text (`T`) is the same for every monomorphization. A cache
//     keyed on the syntactic argument collapses `Run<Dog>`, `Run<Cat>` and
//     `Run<Task<int>>` onto one body; the key must be the instantiation's
//     RESOLVED identity — the arguments as the ACTIVE substitution resolved
//     them, which is how the sibling vtable path derives them.
//
// MEASURED, base vs tip (r30-c1 = 6c2683c4 → this commit):
//   generic_head_skips_its_destructor_body                  base 110 → tip  10
//   generic_head_per_level_base_destructor_still_runs       base 1110 → tip 110
//   concrete_head_is_unchanged                             base  10 → tip  10
//   sync_using_var_still_runs_the_destructor_body          base  10 → tip  10
//   two_instantiations_of_one_generic_get_separate_...     base 220 → tip  20
//   generic_method_monomorphizations_get_separate_...      base 330 → tip  30

#include "support/aot_support.hpp"   // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// `Cell` is the OBSERVABLE for step 2's owned-field cascade: its destructor
// decrements a live count, so a surviving `Cell.n` is exactly "an owned field
// the cascade never disposed". A class with no owned state would have hidden
// the whole defect behind a correct exit code.
const char* kCell =
    "class Cell public static n: int := 0; "
    "  public constructor() Cell.n:=Cell.n+1; end constructor; "
    "  destructor Cell.n:=Cell.n-1; end destructor; end Cell; ";

// The counters: `oda` = the head's `DisposeAsync` body, `odt` = the head's
// destructor body, `bdt` = the BASE level's destructor body.
const char* kK =
    "class K public static oda: int := 0; "
    "  public static odt: int := 0; "
    "  public static bdt: int := 0; "
    "  public constructor() end constructor; end K; ";

// The GENERIC head: a type parameter (so the class is generic), an owned
// field (so the cascade is observable) and a destructor body (so the
// supersession is observable).
const char* kGenericHead =
    "class NG<T>: IAsyncDisposable "
    "  public owned _c: Cell; "
    "  public v: T; "
    "  public constructor(x: T) _c := new Cell(); v := x; end constructor; "
    "  public async method DisposeAsync(): Task K.oda := K.oda + 1; "
    "  end DisposeAsync; "
    "  destructor K.odt := K.odt + 1; end destructor; end NG; ";

// The CONCRETE twin: the same shape, no type parameter. Its path is the
// per-class `bz$NC$DisposeReclaim`, and it must not move.
const char* kConcreteHead =
    "class NC: IAsyncDisposable "
    "  public owned _c: Cell; "
    "  public constructor() _c := new Cell(); end constructor; "
    "  public async method DisposeAsync(): Task K.oda := K.oda + 1; "
    "  end DisposeAsync; "
    "  destructor K.odt := K.odt + 1; end destructor; end NC; ";

// §28.12 step 2's per-level rule needs a BASE level that declares NO
// `DisposeAsync` and HAS a destructor: "at each level of the reclamation chain
// the destructor body is skipped if that level declares or overrides a
// non-`abstract` `DisposeAsync`, and runs if that level does not".
const char* kGenericHeadOverBase =
    "class Base public owned _b: Cell; "
    "  public constructor() _b := new Cell(); end constructor; "
    "  destructor K.bdt := K.bdt + 1; end destructor; end Base; "
    "class NGB<T>: Base, IAsyncDisposable "
    "  public owned _c: Cell; "
    "  public v: T; "
    "  public constructor(x: T) base(); _c := new Cell(); v := x; "
    "  end constructor; "
    "  public async method DisposeAsync(): Task K.oda := K.oda + 1; "
    "  end DisposeAsync; "
    "  destructor K.odt := K.odt + 1; end destructor; end NGB; ";

// THE CACHE-KEY ROW's program. `Dog` is a CLASS (its owned value is disposed
// through the vtable) and `Task<int>` is a Task-family value (disposed by the
// task path) — and BOTH lower to `ptr`, so the erased field-type vector is the
// SAME for `Box<Dog>` and `Box<Task<int>>`. `Dog.n` is the observable that
// says whether the `Box<Dog>` instance's OWNED field was reclaimed.
const char* kTwoInstantiationProgram =
    "module M; import System.Threading; "
    "class Dog "
    "  public static n: int := 0; "
    "  public constructor() Dog.n := Dog.n + 1; end constructor; "
    "  destructor Dog.n := Dog.n - 1; end destructor; "
    "end Dog; "
    "class K "
    "  public static oda: int := 0; "
    "  public static odt: int := 0; "
    "  public constructor() end constructor; "
    "end K; "
    "class Box<T>: IAsyncDisposable "
    "  public owned _t: T; "
    "  public constructor(owned v: T) _t := v; end constructor; "
    "  public async method DisposeAsync(): Task K.oda := K.oda + 1; "
    "  end DisposeAsync; "
    "  destructor K.odt := K.odt + 1; end destructor; "
    "end Box; "
    "class C "
    "  public static async method Run(): Task "
    "    await using var a := new Box<Dog>(new Dog()); "
    "    await using var b := new Box<Task<int>>(Task.FromResult(3)); "
    "  end Run; "
    "  public static async method Main(): Task<int> "
    "    await C.Run(); "
    "    return K.odt * 100 + K.oda * 10 + Dog.n; "
    "  end Main; "
    "end C;";

// ⭐ THE GENERIC-METHOD MONOMORPHIZATION ROW. The head's type argument is
// the METHOD's own type parameter (`Box<T>` inside `Run<T>`), so the head's
// instantiation is not concrete at the binding site — and the SYNTACTIC
// argument text (`T`) is the SAME for every monomorphization of the method.
//
// A cache keyed on the syntactic argument (or on the erased field-type
// vector) therefore collapses `Run<Dog>`, `Run<Cat>` and `Run<Task<int>>` onto
// ONE reclaim body, whose owned-field classification is right for only one of
// them: the `Task<int>` instance is then vtable-disposed instead of
// task-disposed and the program reads through a null pointer and aborts under
// AddressSanitizer. The key must be the instantiation's RESOLVED identity —
// the arguments as the ACTIVE substitution resolved them.
const char* kGenericMethodMonomorphization =
    "module M; import System.Threading; "
    "class Dog "
    "  public static n: int := 0; "
    "  public constructor() Dog.n := Dog.n + 1; end constructor; "
    "  destructor Dog.n := Dog.n - 1; end destructor; "
    "end Dog; "
    "class Cat "
    "  public static n: int := 0; "
    "  public constructor() Cat.n := Cat.n + 1; end constructor; "
    "  destructor Cat.n := Cat.n - 1; end destructor; "
    "end Cat; "
    "class K "
    "  public static oda: int := 0; "
    "  public static odt: int := 0; "
    "  public constructor() end constructor; "
    "end K; "
    "class Box<T>: IAsyncDisposable "
    "  public owned _t: T; "
    "  public constructor(owned v: T) _t := v; end constructor; "
    "  public async method DisposeAsync(): Task K.oda := K.oda + 1; "
    "  end DisposeAsync; "
    "  destructor K.odt := K.odt + 1; end destructor; "
    "end Box; "
    "class M2 "
    "  public static async method Run<T>(owned v: T): Task "
    "    await using var b := new Box<T>(move v); "
    "  end Run; "
    "  public static async method Main(): Task<int> "
    "    await M2.Run<Dog>(new Dog()); "
    "    await M2.Run<Cat>(new Cat()); "
    "    await M2.Run<Task<int>>(Task.FromResult(3)); "
    "    return K.odt * 100 + K.oda * 10 + (Dog.n + Cat.n); "
    "  end Main; "
    "end M2;";
}  // namespace

// THE REPRO ROW. §28.12 step 1: the awaited `DisposeAsync` supersedes the
// destructor body, so the generic head's destructor body must NOT run, and
// step 2's owned-field cascade must still run.
// Observable = odt*100 + oda*10 + cells.
//   fixed:   0*100 + 1*10 + 0 = 10   (DisposeAsync ran, destructor skipped,
//                                    the owned Cell reclaimed)
//   unfixed: 1*100 + 1*10 + 0 = 110  (the destructor body ran as well)
BEZEL_TEST(await_using_generic_head_reclaim_7887,
           generic_head_skips_its_destructor_body) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kK + kGenericHead +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new NG<int>(1); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return K.odt * 100 + K.oda * 10 + Cell.n; "
        "  end Main; "
        "end C;"), 10);
}

// THE PER-LEVEL ROW. §28.12 step 2: the DERIVED level's destructor body is
// skipped (it declares a non-`abstract` `DisposeAsync`) and the BASE level's
// destructor body STILL RUNS (it declares no `DisposeAsync`).
// Observable = odt*1000 + bdt*100 + oda*10 + cells.
//   fixed:   0 + 100 + 10 + 0 = 110
//   unfixed: 1000 + 100 + 10 + 0 = 1110
// A fix that skipped EVERY destructor body would print 10 here, and one that
// ran the derived level's would print 1110 — both are wrong.
BEZEL_TEST(await_using_generic_head_reclaim_7887,
           generic_head_per_level_base_destructor_still_runs) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kK + kGenericHeadOverBase +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new NGB<int>(1); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return K.odt * 1000 + K.bdt * 100 + K.oda * 10 + Cell.n; "
        "  end Main; "
        "end C;"), 110);
}

// THE CONTROL. The ordinary (non-generic) head's path is unchanged: the
// per-class `bz$NC$DisposeReclaim` runs and the destructor body is skipped.
// Observable = odt*100 + oda*10 + cells = 10 on base AND on tip.
BEZEL_TEST(await_using_generic_head_reclaim_7887, concrete_head_is_unchanged) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kK + kConcreteHead +
        "class C "
        "  public static async method Run(): Task "
        "    await using var u := new NC(); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    await C.Run(); "
        "    return K.odt * 100 + K.oda * 10 + Cell.n; "
        "  end Main; "
        "end C;"), 10);
}

// THE CONTROL of §28.12 step 1's other half: a plain `using var` runs the
// destructor body synchronously and does NOT call `DisposeAsync` — "the
// class's two cleanup paths are alternatives, not a sequence".
// Observable = odt*10 + oda = 10 on base AND on tip.
BEZEL_TEST(await_using_generic_head_reclaim_7887,
           sync_using_var_still_runs_the_destructor_body) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kK + kGenericHead +
        "class C "
        "  public static method Run() "
        "    using var u := new NG<int>(1); "
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    C.Run(); "
        "    return K.odt * 10 + K.oda; "
        "  end Main; "
        "end C;"), 10);
}

// ⭐ THE CACHE-KEY ROW. Two instantiations of ONE generic head whose type
// arguments ERASE ALIKE (`Dog` and `Task<int>` both lower to `ptr`) but whose
// owned-field classification DIFFERS.
//
// The per-instantiation reclaim cache must be keyed on the INSTANTIATION's
// IDENTITY, not on the erased field-type vector: with the erased key, ONE
// reclaim body is built for `Box<Dog>` and reused for `Box<Task<int>>`, and
// the second instance is then reclaimed with the first one's classification.
// Observable = odt*100 + oda*10 + Dog.n.
//   fixed:   0 + 20 + 0 = 20  (both heads' destructor bodies skipped, both
//                              `DisposeAsync` bodies ran, the owned `Dog`
//                              reclaimed)
//   unfixed: 200 + 20 + 0 = 220 (the base's DEGRADE ran the destructor bodies)
//   WRONG KEY: 0 + 20 + 1 = 21 (the owned `Dog` was stranded — it was
//                              reclaimed with the `Task<int>` instantiation's
//                              classification) — or an ASan SEGV, when the
//                              `Dog` instantiation is the one that built the
//                              shared body.
BEZEL_TEST(await_using_generic_head_reclaim_7887,
           two_instantiations_of_one_generic_get_separate_reclaim_bodies) {
    BEZEL_EXPECT_EQ(run(kTwoInstantiationProgram), 20);
}

// ⭐ THE GENERIC-METHOD MONOMORPHIZATION ROW. One generic METHOD, three
// monomorphizations, the head's type argument being the method's own type
// parameter.
//
// Observable = odt*100 + oda*10 + (dogs + cats).
//   fixed:   0 + 30 + 0 = 30   (all three heads' destructor bodies skipped,
//                              all three `DisposeAsync` bodies ran, both
//                              owned class fields reclaimed)
//   unfixed: 300 + 30 + 0 = 330 (the base's DEGRADE ran the destructor bodies)
//   WRONG KEY: an AddressSanitizer SEGV (the `Task<int>` instance disposed
//                              through the vtable of the first monomorphization)
//                              — or the owned field stranded.
BEZEL_TEST(await_using_generic_head_reclaim_7887,
           generic_method_monomorphizations_get_separate_reclaim_bodies) {
    BEZEL_EXPECT_EQ(run(kGenericMethodMonomorphization), 30);
}
