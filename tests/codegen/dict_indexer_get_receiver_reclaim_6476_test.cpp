// Codegen — the INDEXER-GET path owes the unbound owned temporary that is its
// receiver a §28.3 discharge, and emitted none (§28.3 *Unbound owned
// temporaries*, §28.11 *Collection element ownership*, stdlib §4.8, v0.338).
//
// THE DEFECT. §28.3 enumerates the borrowing uses of an unbound owned temporary
// — the result of an `owned`-returning call bound to no name, not `move`d, not
// returned — and "as a member or INDEX access" is one of them. In
// `P.MakeDict()["aa"]` the call's `owned Dictionary` binds to no name and the
// index access is its only use, so the rule applies: the temporary "is disposed
// at the end of the full expression in which it is produced, after every use of
// it within that expression has completed". `lowerCollectionIndexerGet` (the
// path that lowers `dict[key]`) is that path's discharge point and emitted
// nothing at all.
//
// MEASURED on this lane's base — 7a0c320f, which ALREADY carries #6470 — over
// 101 iterations, `bezelc --sanitize=address`, `ASAN_OPTIONS=detect_leaks=1`:
//
//   n := n + P.MakeDict()["aa"].Length
//     stdout 303, exit 1
//     SUMMARY: AddressSanitizer: 45955 byte(s) leaked in 606 allocation(s)
//
// 606 = 6 x 101: per iteration the `bz_dict_alloc` header, its bucket array,
// the `bz_dict_set_str` key byte-copy, and three string allocations. The leak
// is the WHOLE TABLE, not one column. The figure is UNCHANGED from the one
// reported before #6470 landed, which is what makes this independent of it.
//
// WHY THIS IS NOT #6470's TWIN, MEASURED RATHER THAN ASSUMED. #6470 discharged
// the `DictionaryEntry` VALUE column on the ESCAPING-VALUE TERMINAL
// (`First`/`Last`/`Single`/`ElementAt`) — a different consumer, and this
// issue's producer is an ordinary `Add`-then-return method rather than a
// terminal. The value column here is separately measured CORRECT: with the
// dictionary bound to a local and the value read into a local that OUTLIVES the
// dictionary's `do ... end` scope, the program prints the right bytes at exit 0
// with no leak and no use-after-free. So §28.11's operations table reading a
// `string` slot through "indexer-get / default iteration" as "reference out" is
// already implemented — the caller holds its own +1 — and this fix is the
// RECEIVER's disposal ALONE. No mint is added anywhere, and no second +1 is
// created for a fix to have to balance.
//
// WHY A SCOPE-END DISPOSAL, NOT AN INLINE FREE. The value handed back is read
// out of the receiver table and the CONSUMER reads it after this function
// returns — `["aa"].Length` loads the `bz_string` header the slot points at.
// Freeing the table inline would free the header the next instruction reads: a
// use-after-free. §28.3 requires exactly this deferral, and for an expression
// statement the full-expression end and the statement end coincide (§21.9), so
// the scope-end pass IS that boundary. Where the indexer-get sits inside a
// larger expression the scope end is merely later than the full-expression
// end, which is the safe direction.
//
// THE GATE IS THE SANCTIONED ONE, NOT A NEW LIST. `isFreshOwnerlessDict-
// MaterializerExpr` / `isFreshOwnerlessArrayMaterializerExpr` /
// `isUnboundOwnedBorrowingArrayCallExpr` are the same three predicates
// `freeUnboundOwnedBorrowingArrayTemp` reads, so this registration and that
// inline reclaim cover the SAME population and cannot disagree: a bound /
// `move`d / returned / owned-argument receiver is none of those three shapes,
// is not registered, and is freed by its own owner — no double-free. The
// `Owning*` family stays excluded by construction and keeps cascading through
// `freeUnboundOwnedOwningCollectionTemp` (#1909).
//
// A LEAK PIN MUST PROVE ITS DETECTOR RAN. `runAot` defaults to
// `ASAN_OPTIONS=detect_leaks=0`, so a leak row on THAT harness is VACUOUS —
// green on a broken tree while showing a clean sanitizer trio. Every leak row
// below therefore runs on `runAotChecked`, which arms LeakSanitizer BY
// CONSTRUCTION: `runAotChecked(source)` is `runAotCheckedImpl(source,
// "runAotChecked", /*detectLeaks=*/true)` (tests/support/aot_support.hpp:3801),
// and that flag is what builds `ASAN_OPTIONS=detect_leaks=1` (:3722-3724).
//
// The knob `BEZEL_AOT_DETECT_LEAKS` (off | report | strict, #4638) is NOT the
// arming mechanism and is deliberately NOT set here. It is read only by the
// `runAot` leak-mode path (tests/support/force_sanitize.hpp); `runAotCheckedImpl`
// never reads it, so setting it changed nothing about these rows. An earlier
// revision of this file wrapped every row in a `LeakEnv("strict")` save/restore
// RAII and claimed in this header that it armed the oracle — a claim that was
// false, and the kind that reads as coverage it does not provide. The RAII is
// gone with the claim.
//
// The sibling `runAotCheckedLenient` is the opposite leg: `detectLeaks=false`,
// so it runs `detect_leaks=0` and CANNOT see a leak. It is used ONLY for the
// crash half of each pair, never for a leak assertion.
//
// Every leak row also asserts `!checkedToolchainSkipped(r)` BEFORE any
// skip-absorbing guard — an `if (skipped) return;` ahead of that assertion
// inverts the oracle, reporting a clean pass on a box whose ASan link was
// unavailable. No `LSAN_OPTIONS` is set: these are found at LSan defaults.
//
// EVERY LEAK ROW IS PAIRED WITH A CRASH ORACLE. `leakedBytes` structurally
// CANNOT see a use-after-free: the abort suppresses LSan's summary, so a UAF
// reads as 0 leaked — the vacuous-oracle trap in its purest form. Each pair runs
// the same program under `runAotCheckedLenient` and asserts it ran to
// completion, printed the expected bytes, and exited 0. A leak fixed by
// OVER-releasing shows up here and nowhere else; over-releasing is a REJECT,
// not a trade.
//
// LEAK BOUNDS ARE AN EXACT ZERO, NOT A CEILING. A ceiling (`leaked <= 2728`)
// passes at 0 as readily as at 7,272, so it cannot distinguish "discharged"
// from "declined" — that is how R31 shipped a tolerated leak and called it
// fixed. Every `leakedBytes` row below asserts `== 0`.
//
// ONE ARM IS CARVED OUT, AND IT IS A CRASH ROW, NOT A LEAK ROW: the RANGE
// SLICE. §28.3 lists "a member or index access" as a borrowing use of an
// unbound owned temporary, but `arr[r]` (§15.6 / §16.3) is not that — it
// MATERIALIZES A NEW ARRAY whose element pointers are copied straight out of
// the receiver (`bz_array_slice` does no `bz_array_retain_strings`), so the
// receiver's storage is still live behind the slice. Discharging the receiver
// on this arm releases each `bz_string` and then the slice's own disposal
// releases it again. MEASURED on 797aff14, over runtime-allocated strings:
//
//   var s: Array<string> := P.MakeArr()[0..1];
//   bezel: fatal: refcount over-release (string) — two releases for one
//   reference; terminating per §28.9                      [exit 134, SIGABRT]
//
// and exit 0 on the base 7a0c320f. A leak must never be traded for a crash, so
// the carve-out is the fix-forward's whole job: the receiver goes UNDISCHARGED
// on this one arm (base-measured 223 bytes / 4 allocations — §28.3's own
// "else it leaks" failure mode, the safe direction) until the slice's missing
// element retain is fixed on its own oracle. That defect is pre-existing and
// independent — on the BASE it ABORTS with a fully BOUND receiver through the
// INDEXER form (`refcount over-release (string)`, exit 134), which no
// registration here touches. The named `Array<T>.Slice(r)` method does not
// abort; it leaks 72 bytes / 2 allocations. So it is NOT fixed in this file,
// and the row below asserts only what this file can own: the slice arm runs,
// exits 0, and prints the right contents.

#include <initializer_list>
#include <regex>
#include <string>

#include "support/aot_support.hpp"
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The harness SKIPS (loudly, counted) when clang is absent; a row that asserts
// through that would be asserting on an unarmed oracle. A sanitizer finding is
// NOT a skip — "sanitizer fired" means it ran and found something.
bool skipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("SKIP") == std::string::npos) continue;
        if (d.message.find("sanitizer fired") != std::string::npos) return false;
        return true;
    }
    return false;
}

// 101 iterations — a one-shot leak repro is a vacuity trap: one evaluation's
// table is small enough to hide under LSan noise, and the defect repeats once
// per evaluation. Every number quoted in this file is a 101-iteration
// measurement.
const char* kIterations = "i < 101";

// THE ISSUE'S REPRO'S PRODUCER, byte for byte. The value is a CONCATENATION
// producer (`"va" + "a"`), so it allocates its own +1 rather than aliasing an
// immortal literal — a leak on this shape is therefore several allocations per
// iteration, which is the 606 the base measured, and it is large enough to
// clear LSan noise rather than hide under it.
std::string strValProducer() {
    return
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, string> "
        "    var d: Dictionary<string, string> := new Dictionary<string, string>(); "
        "    d.Add(\"aa\", \"va\" + \"a\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; ";
}

// Bytes LSan reports for `program`, or 0 when it reports none. The trap this
// shape hides is why every caller pairs it with a crash row — see the header.
long leakedBytes(const std::string& program) {
    // `runAotChecked` arms LSan by construction (see the header). No env knob.
    JitResult r = runAotChecked(program);
    static const std::regex kLeak(R"(AddressSanitizer: (\d+) byte\(s\) leaked)");
    for (const auto& d : r.diagnostics.diagnostics()) {
        std::smatch m;
        if (std::regex_search(d.message, m, kLeak)) return std::stol(m[1]);
    }
    return 0;
}

// The CRASH oracle: run to completion, correct stdout, exit 0. `r.ran` is the
// load-bearing assertion — false iff the generated program died on a memory
// error. A row whose printed CONTENT is more than one value uses the
// `expectRunsCleanAll` form below: EVERY expected value must appear in stdout,
// so a row cannot pass by merely avoiding the fault — a program that produced
// an empty or short-circuited slice, or one whose elements came from somewhere
// else, fails here.
void expectRunsCleanAll(const std::string& program,
                        std::initializer_list<const char*> expected) {
    JitResult r = runAotCheckedLenient(program);
    // Loud-first, silent-second, in that order.
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    for (const char* e : expected)
        BEZEL_EXPECT_TRUE(r.stdoutText.find(e) != std::string::npos);
}

void expectRunsClean(const std::string& program, const char* expected) {
    expectRunsCleanAll(program, {expected});
}

// A LEAK-oracle row: LSan armed by `runAotChecked` itself, program runs clean.
void expectNoLeak(const std::string& program, const char* expected) {
    JitResult r = runAotChecked(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expected) != std::string::npos);
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("byte(s) leaked") == std::string::npos);
    }
}
// ---------------------------------------------------------------------------

// THE ISSUE'S OWN REPRO, ASSERTED AT ZERO. `n` accumulates `.Length` so the
// value is observably USED — a dead store could be optimized into a shape that
// never materializes the reference, which would make this row vacuously green.
//
// MEASURED on the base (7a0c320f): exit 1, stdout 303,
// 45955 byte(s) leaked in 606 allocation(s).
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           fresh_dict_indexer_get_releases_its_receiver_spec_28_3) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + P.MakeDict()[\"aa\"].Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "303");
}

// THE BOUND-RECEIVER CONTROL — the arm this fix must NOT have widened. With
// the dictionary bound to a local, the local's own scope-end pass owns the
// table. If this fix's registration were admitted for a bound receiver the
// table would be freed TWICE: a double-free, which this row catches directly
// (`expectRunsClean` fails, `r.ran` false) rather than by a leak count.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           a_bound_dict_receiver_is_freed_exactly_once_spec_28_3) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var d: Dictionary<string, string> := P.MakeDict(); "
        "      n := n + d[\"aa\"].Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    expectNoLeak(program, "303");
}

// THE INT-VALUED TWIN. A `Dictionary<string,int>` has no co-owned +1 on its
// value column at all, so there is nothing for the free to release there — this
// row is clean on BOTH arms and is what says the fix's discharge is installed
// by column category and not sprayed. It is also a leak oracle in its own
// right: the base leaked the table here too (MEASURED 42319 B / 404 allocs over
// 101 iterations), so it is a genuine fail-without row, not a vacuous control.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           fresh_dict_int_valued_indexer_get_releases_its_receiver_spec_28_3) {
    std::string program =
        "module M; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, int> "
        "    var d: Dictionary<string, int> := new Dictionary<string, int>(); "
        "    d.Add(\"aa\", 7); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + P.MakeDict()[\"aa\"]; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "707");
}

// THE CLASS-VALUED-V TWIN. §28.11's element-category table gives a class slot
// its own row ("borrow out" on indexer-get), so this is the shape where the
// free's value-column disposer must NOT be aimed at a co-owned +1 that does not
// exist. MEASURED on the base: 43935 B / 505 allocs over 101 iterations, so
// this is a real fail-without row and not a control.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           fresh_dict_class_valued_indexer_get_releases_its_receiver_spec_28_3) {
    std::string program =
        "module M; "
        "class Box public N: int := 42; end Box; "
        "class P public "
        // The Box is a `Main` local, not a fresh `new Box()` argument: an
        // unbound owned temporary passed to a BORROWING `Dictionary.Add` is
        // itself an undischarged §28.3 temporary, a SEPARATE pre-existing
        // defect (filed alongside this fix). Binding it here keeps this row's
        // leak count attributable to the receiver alone.
        "  static method MakeDict(b: Box): owned Dictionary<string, Box> "
        "    var d: Dictionary<string, Box> := new Dictionary<string, Box>(); "
        "    d.Add(\"aa\", b); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var b := new Box(); "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + P.MakeDict(b)[\"aa\"].N; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "4242");
}

// THE ESCAPING-VALUE ROW — the load-bearing soundness check, and the one a
// leak count cannot make. The value read out of the temporary dictionary is
// stored into a local in an ENCLOSING scope and read AFTER the inner `do ... end`
// that produced the dictionary has closed. §28.11 reads this as "reference out",
// so the value must survive its table: if the discharge were installed with the
// value column pointed at the caller's reference (rather than at the table's own
// co-owned +1), this program reads freed memory.
//
// It is also a leak oracle: the table itself must still be reclaimed, so this
// asserts BOTH halves at once — `expectNoLeak` demands a clean sanitizer run
// with no leaked bytes, and `expectRunsClean` demands the escaped value still
// reads `vaa`.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           a_value_read_out_of_a_freed_receiver_is_still_readable_spec_28_11) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var s: string := \"\"; "
        "    do "
        "      s := P.MakeDict()[\"aa\"]; "
        "    end do; "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    expectNoLeak(program, "vaa");
    expectRunsClean(program, "vaa");
}

// THE THROWN-KEY PATH. §28.3's exception-path paragraph requires an
// already-produced, not-yet-consumed unbound owned temporary to be discharged
// DURING UNWINDING, and the absent-key guard the indexer-get raises
// (`KeyNotFoundException`, stdlib §4.8) leaves the expression mid-flight with
// the table already produced. This row reads a key that is NOT in the
// dictionary, so the guard throws every iteration: a program that completes
// normally would prove nothing here.
//
// `expectRunsClean` asserts `r.ran` and exit 0, so the harness must report the
// uncaught KeyNotFoundException as a non-zero exit WITHOUT the sanitizer
// finding anything — `runAotCheckedLenient` is what makes an uncaught exception
// a legitimate non-zero exit rather than an oracle failure (tests/README).
//
// THIS IS A SHAPE ORACLE, NOT A LEAK ORACLE, AND MUST NOT BE COUNTED AS ONE.
// An earlier revision of this file also asserted the absence of "byte(s) leaked"
// here, on the stated grounds that "the temporary is discharged by the unwind".
// That assertion could never fire, for TWO INDEPENDENT reasons:
//
//   1. `runAotCheckedLenient` is `runAotCheckedImpl(source, …,
//      /*detectLeaks=*/false)` (aot_support.hpp:3825), which builds
//      `ASAN_OPTIONS=detect_leaks=0` (:3722-3724). LeakSanitizer is switched OFF
//      at runtime; no leak text can be produced on this leg at all.
//   2. Even with LSan armed, the program exits through `bz_unhandled_exception`,
//      which calls `_exit(70)` (src/runtime/bezel_runtime_itable.cpp:260-269,
//      §33.3 / #1609) — `_exit` bypasses LSan's atexit hook, so the summary is
//      never printed even on the arming leg.
//
// So the row was green on the base AND on the tip for a reason that had nothing
// to do with the receiver. The real unwind coverage is the CAUGHT row below,
// which is the same shape with the throw handled, so neither `_exit` nor
// `detect_leaks=0` applies and LSan genuinely reports.
void expectRunsAndExitsNonZero(const std::string& program) {
    JitResult r = runAotCheckedLenient(program);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    if (skipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.exitCode != 0);
    // Only the CRASH half is available here — see the note above. The
    // generated-code AddressSanitizer is still active under `detect_leaks=0`,
    // so a use-after-free on the unwind edge is still visible.
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("use-after-free") == std::string::npos);
    }
}

BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           an_uncaught_absent_key_throw_exits_nonzero_spec_28_3) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    while " + std::string(kIterations) + " do "
        "      Console.WriteLine(P.MakeDict()[\"missing\"]); "
        "      i := i + 1; "
        "    end while; "
        "    return 0; "
        "  end Main; "
        "end C; ";
    expectRunsAndExitsNonZero(program);
}

// THE REAL §28.3 EXCEPTION-PATH ROW — and the one that carries the leak
// assertion the uncaught row above could not.
//
// §28.3's exception-path paragraph discharges an already-produced,
// not-yet-consumed unbound owned temporary during unwinding. The registration
// is placed at the PRODUCTION rather than after the absent-key decode precisely
// so this edge is covered: the table exists when `emitGuardThrow` branches to
// the scope's unwind pass, and without a registration at the production there
// would be nothing to discharge.
//
// The throw is CAUGHT rather than uncaught, and that is the whole point: the
// uncaught path reaches `bz_unhandled_exception`'s `_exit(70)`, which bypasses
// LSan's atexit hook, and its harness leg pins `detect_leaks=0` besides — so an
// uncaught row can never observe a leak. Catching the exception returns the
// program to a normal `exit`, LSan runs its atexit hook, and the oracle is real.
//
// MEASURED at 101 iterations, `bezelc --sanitize=address`:
//   base 7a0c320f : 45955 byte(s) leaked in 606 allocation(s)   <- the issue's figure
//   tip  797aff14 : 0 leaked, stdout 303
// `n` accumulates 3 per caught throw, so the catch arm is observably TAKEN — a
// silently-swallowed exception would make this row green without exercising the
// unwind at all.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           a_caught_absent_key_throw_discharges_the_receiver_spec_28_3) {
    std::string program =
        "module M; import System.Collections.Generic; "
        "class P public "
        "  static method MakeDict(): owned Dictionary<string, string> "
        "    var d: Dictionary<string, string> := new Dictionary<string, string>(); "
        "    d.Add(\"aa\", \"va\" + \"a\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      try "
        "        n := n + P.MakeDict()[\"zz\"].Length; "
        "      catch e: KeyNotFoundException => "
        "        n := n + 3; "
        "      end; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    // 101 catches x 3. If the catch arm were not taken, `n` would be 0.
    expectRunsClean(program, "303");
}

// THE DISCARD ROW. `_ := P.MakeDict()["aa"];` is §17.13's discard: the
// announcement is the discard itself, and the receiver temporary remains
// undischarged at the statement's end, so §28.3's full-expression rule (not the
// named-value regime) governs it. This is the arm where nothing consumes the
// value at all — the sharpest test that the fix discharges the RECEIVER and
// does not merely rely on some consumer to do it. MEASURED on the base:
// 45955 B / 606 allocs, identical to the issue's repro.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           a_discarded_indexer_get_still_releases_its_receiver_spec_28_3) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    while " + std::string(kIterations) + " do "
        "      _ := P.MakeDict()[\"aa\"]; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(0); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "0");
}

// THE ARRAY TWIN. The same missing discharge on the sibling collection family:
// `P.MakeArr()[0]` over an unbound `owned Array<string>`. §28.3's rule is
// uniform across the source of the temporary — "there is no special-cased list
// of method names" — so the same bound applies. MEASURED on the base:
// 22220 B / 404 allocs over 101 iterations.
//
// This row also pins that the fix's `buildCollectionTempDisposal` classifier
// picks `bz_array_free` for an array-backed receiver and not the dict free the
// dictionary rows exercise; a mix-up would be a wild free, which
// `expectRunsClean` catches directly.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           fresh_array_indexer_get_releases_its_receiver_spec_28_3) {
    std::string program =
        "module M; "
        "class P public "
        "  static method MakeArr(): owned Array<string> "
        "    var d: Array<string> := new Array<string>(); "
        "    d.Add(\"va\" + \"a\"); "
        "    return d; "
        "  end MakeArr; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + P.MakeArr()[0].Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "303");
}

// THE RANGE-SLICE ARM — the CRASH row this fix-forward exists for, and the one
// the review of 797aff14 rejected the parent commit over.
//
// `P.MakeArr()[0..1]` is the same unbound owned temporary reaching the same
// indexer-get path, but the index is a Range, so §15.6 / §16.3 make the whole
// expression a SLICE: `lowerArrayRangeSlice` calls `bz_array_slice`, which
// memcpy's the element pointers into a new array and retains nothing. The
// receiver and the slice then co-own every element. With the receiver's §28.3
// registration ahead of the slice early-return (797aff14), the scope-end pass
// releases each element and the slice's own disposal releases it a second time.
//
// RUNTIME-ALLOCATED STRINGS ARE LOAD-BEARING. `a.Add($"elem-{i}")` interpolates
// at run time and the result is refcounted; a plain literal is immortal and
// this arm exits 0 with the fault present — measured, and it is why this row
// must not be written with one.
//
// The assertion is CONTENT, not survival: `n` accumulates the sliced Count and
// `m` accumulates the length of the sliced first element, both 101 times, and
// `elem-0` itself is printed, so an empty or short-circuited slice fails here
// even though it would not fault. MEASURED: 797aff14 SIGABRTs (exit 134) on the
// first iteration; 7a0c320f and this fix print 202 / 606 / elem-0 at exit 0.
//
// NO LEAK ASSERTION HERE, DELIBERATELY. The receiver is undischarged on this arm
// (base-measured 223 B / 4 allocs), so a `leakedBytes == 0` assertion would be
// false and would turn a correct fix into a red row. That leak belongs to the
// slice's missing `bz_array_retain_strings`, which reproduces on the base with
// a BOUND receiver and through `.Slice(r)` too and is filed on its own oracle.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           a_range_sliced_indexer_get_does_not_double_release_its_receiver_spec_28_3) {
    std::string program =
        "module M; "
        "class P public "
        "  static method MakeArr(): owned Array<string> "
        "    var a: Array<string> := new Array<string>(); "
        "    for var i in 0..<3 do a.Add($\"elem-{i}\"); end for; "
        "    return a; "
        "  end MakeArr; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    var m := 0; "
        "    while " + std::string(kIterations) + " do "
        "      var s: Array<string> := P.MakeArr()[0..1]; "
        "      n := n + s.Count; "
        "      m := m + s[0].Length; "
        "      Console.WriteLine(s[0]); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    Console.WriteLine(m); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    // 101 x (Count 2) = 202; 101 x len("elem-0") = 606.
    expectRunsCleanAll(program, {"elem-0", "202", "606"});
}

// THE SCALAR-METHOD CONTROL — the arm that was ALREADY correct and must stay
// correct. `P.MakeDict().Count` is the same unbound temporary over the same
// dictionary, consumed by a method rather than by an index; its discharge rides
// `freeUnboundOwnedBorrowingArrayTemp` on the call path and has emitted
// `bz_dict_free` since #6352. If this fix's registration were wrongly admitted
// for that shape — or if the two ever disagreed about the population — the
// table would be freed twice here. This row is the differential that says the
// new registration is NARROWER than the existing one (it is on the indexer-get
// path, which the call path never reaches).
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           a_scalar_method_on_a_fresh_dict_is_freed_exactly_once_spec_28_3) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + P.MakeDict().Count; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    expectNoLeak(program, "101");
}

// THE CLASS-KEYED TABLE TWIN. The class-typed-V row above exercises the VALUE
// column; this one exercises the KEY column, which is the other half of
// §28.11's element-category table and the half whose disposal is a plain
// `free(e.key)` of the table's own byte-copy rather than a value disposer. If
// the registration's classifier (`buildCollectionTempDisposal`, keyed on the
// `V` type argument alone) had aimed the class disposer at the key column, this
// row would double-free the key.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           fresh_dict_class_keyed_indexer_get_releases_its_receiver_spec_28_11) {
    std::string program =
        "module M; "
        "class Box public N: int := 42; end Box; "
        "class P public "
        "  static method MakeDict(k: Box): owned Dictionary<Box, string> "
        "    var d: Dictionary<Box, string> := new Dictionary<Box, string>(); "
        "    d.Add(k, \"va\" + \"a\"); "
        "    return d; "
        "  end MakeDict; "
        "end P; "
        "class C public "
        "  static method Main(): int "
        "    var k := new Box(); "
        "    var i := 0; "
        "    var n := 0; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + P.MakeDict(k)[k].Length; "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "303");
}

// THE CLOSURE ROW — the §27.5 half of the issue's report ("it also reproduces
// through a lambda"). A BLOCK-BODIED lambda body is lowered through
// `lowerScopedBlock`, which opens its own disposal scope and emits its
// disposals, so the registration lands and fires. The EXPRESSION-bodied form
// (`lambda() => P.MakeDict()["aa"].Length`) is measured still leaking and is
// NOT covered here: `lowerLambda` clears `disposeStack_` and pushes no scope
// for that body form, so there is nothing to register into. Filed separately.
BEZEL_TEST(dict_indexer_get_receiver_reclaim_6476,
           fresh_dict_indexer_get_in_a_block_bodied_lambda_releases_its_receiver_spec_28_3) {
    std::string program =
        strValProducer() +
        "class C public "
        "  static method Main(): int "
        "    var i := 0; "
        "    var n := 0; "
        "    var f := lambda() => do "
        "      var x := P.MakeDict()[\"aa\"].Length; "
        "      return x; "
        "    end; "
        "    while " + std::string(kIterations) + " do "
        "      n := n + f(); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C; ";
    BEZEL_EXPECT_EQ(leakedBytes(program), 0);
    expectRunsClean(program, "303");
}

}  // namespace