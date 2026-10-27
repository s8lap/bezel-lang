// Regression test for #3605 (§29.7 / §29.2 / §29.11, v0.286): a NAMED
// iterator-format local bound to a bare-identifier property read (`var
// seq: IEnumerable<T> := Seq;` with `Seq` declaring in its own class) leaks
// the wrapper frame on §29.7 abandonment.
//
// The §29.7 *Disposal of partially-consumed iterators* snapshot iterates
// every live `Disposal` at the suspend point and adds an
// `IterCleanup` action keyed on either a frame field (the for-in's own
// implicit enumerator/source, §29.11 #944) or a node lookup in `locals_`
// (the fallback added by #3468 for an iterator-format NAMED local). The
// fallback asks `propReadIterWrappedExprs_` whether the local-decl's
// initializer actually MINTED the wrapper frame, so a slot-1 Dispose is
// registered ONLY when the local owns one.
//
// #3468's claim path was written for the §29.2 property-read-as-
// MemberAccessExpr shape (`this.Seq` and friends) and reads the wrap-creator
// report through a `dynamic_cast<ast::MemberAccessExpr>(init)` guard. Inside
// the declaring class itself the read is bare `Seq` (IdentifierExpr), and
// `wrapPropertyArrayAsIEnumerable` (LlvmBackend_Exprs.cpp:6210) wraps it
// through the bare-`this` computed-getter arm, keying the report on the
// IdentifierExpr. The MemberAccessExpr-only claim path never saw the entry,
// so the local claimed no slot-1 disposal: the wrapper frame leaked one per
// abandonment — measured 12800 B / 400 allocations in `bz$iter$
// $Src$Walk$MoveNext` on c10-l3 HEAD (#3605's exact measurement).
//
// The fix extends the claim to BOTH the MemberAccessExpr and bare-Identifier
// spellings of an iterator-format-typed property read, lifting the
// pure-IEnumerable verdict out of the inner arm so both checks see the same
// head classifier. Wider borrows (`Seq: IReadOnlyList<T>` etc.) stay out:
// only `IEnumerable<T>` / `IOwningEnumerable<T>` heads participate in the
// wrap-creator's set, and the pre-existing `declIsPureIEnum` guard carries
// that exclusion.
//
// PART 2 (§29.7 / §29.2 / §29.11 / §23.4, v0.286): a THIRD declaring-class
// spelling of the same iterator-format property read — the indexer form
// `this[0]`, an IndexExpr resolving to the indexer's PropertySymbol.
// `wrapPropertyArrayAsIEnumerable` (LlvmBackend_Assign.cpp:2301) is the
// third call site and records the FRAME in `propReadIterWrappedExprs_`
// keyed on the IndexExpr, but the IdentifierExpr-only claim arm never sees
// that key, and `computedGetterOf` rejects indexers explicitly so the wider
// MemberAccessExpr claim above is also unreachable. The fix's IndexExpr arm
// inlines the indexer-allowing expression-body predicate and consults the same
// `propReadIterWrappedExprs_` set, so the third call site is now visible to
// the §29.7 fallback.
//
// ORACLE. LeakSanitizer is the oracle — every leaked object here is a
// compiler-synthesised wrapper (32 B per `{vtable, arr, idx, esz}` for
// arrenum.ptr, exactly the #3605 measurement), so no user destructor
// counts it. The program AMPLIFIES the abandonment to 400 rounds so the
// leak cannot hide as a still-stack-reachable single allocation, and
// asserts t on stdout so a program that stopped producing the right answer
// is not silently accepted.
//
// Spec authority: §29.7 *Disposal of partially-consumed iterators*,
// §29.2 *Desugaring of `for`*, §29.11, §23.4 *Indexers*, stdlib
// §4.12–§4.13 (v0.286).

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

constexpr int kRounds = 400;

// The issue's exact repro: `var seq: IEnumerable<Item> := Seq;` (the bare-
// identifier property read), then a yielding for-in over `seq`, then an
// explicit `seq.Dispose()` followed by another `yield return`. Consumer
// `break`s after three yields (3, 4, 9) so the iterator suspends at the
// post-for-in yield — state 2 — and the using-var on `s` then drives
// `Walk$Dispose` with that curstate. Pre-fix, the §29.7 snapshot for state
// 2 dropped the seq wrapper's slot-1 entry, so the iter-frame was freed
// without releasing the arrenum.ptr.
std::string program() {
    return
        "module M;\n"
        "class Item\n"
        "  public V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "end Item;\n"
        "class Src\n"
        "  private owned _a: OwningArray<Item>;\n"
        "  public constructor()\n"
        "    _a := new OwningArray<Item>();\n"
        "    _a.Add(new Item(3));\n"
        "    _a.Add(new Item(4));\n"
        "  end constructor;\n"
        "  public property Seq: IEnumerable<Item> => _a;\n"
        "  public method Walk(): IEnumerable<int>\n"
        "    var seq: IEnumerable<Item> := Seq;\n"
        "    for var x in seq do\n"
        "      yield return x.V;\n"
        "    end for;\n"
        "    seq.Dispose();\n"
        "    yield return 9;\n"
        "  end Walk;\n"
        "end Src;\n"
        "class Main\n"
        "  public static method Run(): int\n"
        "    using var s := new Src();\n"
        "    var t := 0;\n"
        "    var n := 0;\n"
        "    for var v in s.Walk() do\n"
        "      t := t * 10 + v;\n"
        "      n := n + 1;\n"
        "      if n = 3 then break; end if;\n"
        "    end for;\n"
        "    return t;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < " + std::to_string(kRounds) + " do\n"
        "      t := t + Run();\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

// Consumer yields 3, 4, 9 in turn before the n=3 break fires: t per Run is
// 0 + 3 = 3 -> 3*10 + 4 = 34 -> 34*10 + 9 = 349; over kRounds = 400 rounds
// of an exit-on-third yield, Run returns 349 each time -> t = 400 * 349
// = 139600.
constexpr const char* kExpectOut = "t=139600";

// PART 2: the indexer spelling of #3605 — `property [i: int]:
// IEnumerable<Item> => _a;` and `var seq: IEnumerable<Item> := this[0];`.
// Structurally identical to the bare-identifier case (same OwningArray<Item>
// backing, same yield-3-4-9 + explicit Dispose + later yield), but the
// read is an IndexExpr — the third declaring-class spelling
// `wrapPropertyArrayAsIEnumerable` records (`LlvmBackend_Assign.cpp:2301`).
// Pre-amendment (a85a7c1): byte-identical 12800 B / 400 leak, the same
// frame as the IdentifierExpr spelling.
std::string programIndexer() {
    return
        "module M;\n"
        "class Item\n"
        "  public V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "end Item;\n"
        "class Src\n"
        "  private owned _a: OwningArray<Item>;\n"
        "  public constructor()\n"
        "    _a := new OwningArray<Item>();\n"
        "    _a.Add(new Item(3));\n"
        "    _a.Add(new Item(4));\n"
        "  end constructor;\n"
        "  public property [i: int]: IEnumerable<Item> => _a;\n"
        "  public method Walk(): IEnumerable<int>\n"
        "    var seq: IEnumerable<Item> := this[0];\n"
        "    for var x in seq do\n"
        "      yield return x.V;\n"
        "    end for;\n"
        "    seq.Dispose();\n"
        "    yield return 9;\n"
        "  end Walk;\n"
        "end Src;\n"
        "class Main\n"
        "  public static method Run(): int\n"
        "    using var s := new Src();\n"
        "    var t := 0;\n"
        "    var n := 0;\n"
        "    for var v in s.Walk() do\n"
        "      t := t * 10 + v;\n"
        "      n := n + 1;\n"
        "      if n = 3 then break; end if;\n"
        "    end for;\n"
        "    return t;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < " + std::to_string(kRounds) + " do\n"
        "      t := t + Run();\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

void expectClean(const JitResult& r, const char* expectOut) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expectOut) != std::string::npos);
}

}  // namespace

// --- the issue's own repro: bare-identifier property read into a named
// iterator-format local, for-in over it, explicit `.Dispose()`, later yield,
// consumer `break` mid-iteration. Pre-fix: 12800 B / 400 leak in
// `bz$iter$M.Src$Walk$MoveNext` (#3605's measurement).
BEZEL_TEST(named_local_property_read_source_abandonment_3605,
           bare_identifier_property_local_disposed_on_abandonment_29_7) {
    JitResult r = runAotChecked(program());
    expectClean(r, kExpectOut);
}

// --- PART 2: indexer property read into a named iterator-format local.
// Same shape as the bare-identifier case, but the read is `this[0]`
// (IndexExpr resolving to the indexer's PropertySymbol). Pre-amendment
// (a85a7c1): the IdentifierExpr-only claim arm never sees the IndexExpr
// key in `propReadIterWrappedExprs_`, and `computedGetterOf` rejects
// indexers, so the MemberAccessExpr claim above is also unreachable for
// this shape — byte-identical 12800 B / 400 leak. Post-amendment: the
// IndexExpr arm consults the same set with an inline
// indexer-allowing expression-body predicate, and the §29.7 fallback
// registers slot-1 disposal correctly.
BEZEL_TEST(named_local_property_read_source_abandonment_3605,
           indexer_property_local_disposed_on_abandonment_29_7) {
    JitResult r = runAotChecked(programIndexer());
    expectClean(r, kExpectOut);
}
