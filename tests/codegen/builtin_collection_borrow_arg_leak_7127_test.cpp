// fix #7127 (and #6889) — a named `Array<T>` local passed to a BUILT-IN
// member that READS it is NEVER DISPOSED: the compiler-inserted scope-end
// free of the array does not run, and the callee does not dispose it either.
//
// ── THE DEFECT ──────────────────────────────────────────────────────────────
//
// stdlib §4.4 (v0.343) is the anchor: `Array<T>` is TRIVIAL-DISPOSE
// (language spec §28.9) — "the backing storage is part of the instance's
// single allocation, released with it by the compiler-inserted free at the
// array's owner's release point — and the elements are not disposed". ⇒ the
// scope-end release is what must reclaim the backing storage, and it did not
// run.
//
// MEASURED on this tree's parent (`bezelc --sanitize=address`, LSan):
//
//   `var a: Array<string> := ["x","y"]; string.Join(",", a);`
//       → 208 B / 2 allocations leaked (the `bz_array_alloc` header at
//         src/runtime/bezel_runtime_collections.cpp:252 and its backing store
//         at :257);
//   `var a: Array<string> := ["x","y"]; Console.WriteLine(a);`
//       → the SAME 208 B / 2 allocations — so #6889 and #7127 are ONE site;
//   `var b: Array<char> := ['a','b','c']; string.From(b, 0, 3);`
//       → 144 B / 2 allocations;
//   `var cs: Array<char> := [' ']; "abc".Trim(cs);`
//       → 144 B / 2 allocations.
//
// ⇒ ⇒ ⇒ THIS IS A LEAK, NOT A TRANSFER. The local is NOT consumed: after
// `string.Join(",", a)` the program still reads `a.Count` and gets the right
// value (row 12 below pins that), so no ownership moved and no
// use-after-move is diagnosed. The scope-end release was suppressed WITHOUT
// any ownership transfer ⇒ the array's owner released nothing ⇒ §28.9's
// "released exactly once" is violated.
//
// ── WHY THE SCOPE-END FREE WAS SUPPRESSED ───────────────────────────────────
//
// The array local's scope-end free is armed only for a local the escape walk
// proves never ESCAPES (`collectBorrowingArrayFreeEligible` →
// `borrowingArrayFreeEligible_`). The walk's #1430 relaxation admits a bare
// identifier in a PLAIN (no-modifier) formal position of a RESOLVED
// user/imported method as a CALL-BOUNDED borrow — but a BUILT-IN callee has
// no resolved MethodSymbol (`ctx.exprSymbol` on `string.Join` /
// `Console.WriteLine` is null: those members are matched structurally by the
// lowerings), so the `ms` path never fired and every builtin argument fell
// through to the walk's conservative ESCAPE.
//
// ── WHY THE CALL-BOUNDED-BORROW READING IS THE SPEC'S ───────────────────────
//
// §22.3 (v0.343): a PLAIN (non-`owned`) formal is a BORROW — the `owned`
// modifier is what transfers ownership. stdlib §9.6 declares
// `Join(separator: string, items: IEnumerable<string>)`,
// `From(buffer: Array<char>, start: int, length: int)` and
// `Trim(chars: IEnumerable<char>)` with NO `owned` marker, and stdlib §2.1
// declares `Console.Write(value: …)` / `Console.WriteLine(value: …)` the same
// way ⇒ every one of those positions is a BORROW and the CALLER keeps the
// obligation ⇒ the scope-end free must run. (stdlib §3.1 states the same fact
// for the `StringBuilder` family's own `args: Array<Object>` parameter:
// "`AppendFormat`'s `args` is a call-bounded borrow".)
//
// ── THE FIX, AND WHY IT IS NOT "EVERY BUILTIN CALLEE" ───────────────────────
//
// The relaxation is NOT the blanket rule "a builtin callee borrows its
// argument", because a builtin CAN store its argument: a COLLECTION member
// stores it into an element slot (`Array<T>.Add(item: T)`, stdlib §4.4).
// MEASURED on this tree's parent:
//
//   `var l: Array<Array<int>> := []; do var a: Array<int> := [1,2];
//    l.Add(a); end do; Console.WriteLine(l[0][0]);`
//
// COMPILES, and LEAKS 144 B — the borrow escapes the inner scope into the
// outer container and the compiler does not reject it. Freeing `a` at its
// scope end there would turn that leak into a heap-use-after-free, so the
// fix's relaxation is admitted ONLY for the builtins whose lowering READS the
// argument and provably never stores it — keyed on the SAME structural shapes
// the lowerings key on (the `isConsoleNonRetainingArg` discipline). Row 14
// below pins that the container-insert shape is UNCHANGED (still a leak, not
// a use-after-free).
//
// An OWNING collection's element slot requires `owned` (stdlib §4.5's
// `OwningArray<T>.Add(owned item: T)`), so the compiler REJECTS a plain
// argument there — MEASURED: "a borrow cannot be passed where an `owned`
// value is required" — and the call-site `owned`/`move` spelling is excluded
// by the `a.modifier.empty()` gate at the call site.
//
// ── THE ORACLE ──────────────────────────────────────────────────────────────
//
// Every leak row runs under `runAotChecked`, which arms AddressSanitizer
// over the GENERATED module AND LeakSanitizer at exit (`ASAN_OPTIONS=
// detect_leaks=1`), and turns either into an error diagnostic with `ran`
// false. A leak therefore fails the row.
//
// ⚠ EVERY LEAK ROW IS LOOPED (200 iterations). A ONE-SHOT repro can leave
// the array's pointer in a live stack slot at exit and evade LSan's
// reachability scan — this repo has been bitten by that (CLAUDE.md's
// vacuous-oracle rule). The one-shot spellings appear only in the VALUE
// rows, which assert the printed output, not the leak.
//
// ⚠ THE VALUES ARE PINNED, NOT JUST THE EXIT CODES. A process exit status
// is masked to 0..255 and cannot carry a string value, so every row that
// can assert a value does so through the captured stdout.
//
// ⚠ THE LEAK COUNTER IS A NUMBER, NOT A VERDICT. Row 11 runs the repro
// through the harness's report-mode leak census (`BEZEL_AOT_DETECT_LEAKS=
// report`) and asserts the process-wide counter did not MOVE — a literal
// "leak counter is 0" for this row — so the row cannot be satisfied by an
// oracle that is not armed.

#include <atomic>
#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/force_sanitize.hpp"
#include "test_harness.hpp"

namespace {

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row (the #4638 knob). Required,
// not tidiness: the gate may run a whole shard with the knob set, and row 11
// asserts the report-mode behaviour specifically. Restoring means the rows are
// order- and ambient-independent either way.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

std::size_t leakCount() {
    return bezel::test::aotLeakDetections().load(std::memory_order_relaxed);
}

// ── THE PROGRAMS ───────────────────────────────────────────────────────────
//
// Every leak row loops 200 times over a FRESH array, so a leak is 200
// distinct allocations (the LSan report would name them all) and the loop
// body is the whole story. The VALUE rows are the one-shot spellings.

// §9.6 `string.Join(separator: string, items: IEnumerable<string>): string`
// — the `items` parameter is the ARRAY the defect is about.
constexpr const char* kJoinLoop =
    "module M; class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..<200 do "
    "    var a: Array<string> := [\"x\", \"y\"]; "
    "    var s: string := string.Join(\",\", a); "
    "    total := total + s.Length; "
    "  end; "
    "  Console.WriteLine(total); "
    "  return 0; "
    "end Main; end C;";
constexpr const char* kJoinValue =
    "module M; class C public static method Main(): int "
    "  var a: Array<string> := [\"x\", \"y\"]; "
    "  var s: string := string.Join(\",\", a); "
    "  Console.WriteLine(s); "
    "  return 0; "
    "end Main; end C;";

// §2.1 `Console.WriteLine(value: …)` — the #6889 instance of the SAME site.
constexpr const char* kConsoleLoop =
    "module M; class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..<200 do "
    "    var a: Array<string> := [\"x\", \"y\"]; "
    "    Console.WriteLine(a); "
    "    total := total + a.Count; "
    "  end; "
    "  Console.WriteLine(total); "
    "  return 0; "
    "end Main; end C;";
constexpr const char* kConsoleValue =
    "module M; class C public static method Main(): int "
    "  var a: Array<string> := [\"x\", \"y\"]; "
    "  Console.WriteLine(a); "
    "  return 0; "
    "end Main; end C;";

// §9.6 `string.From(buffer: Array<char>, start: int, length: int): string`.
constexpr const char* kFromLoop =
    "module M; class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..<200 do "
    "    var b: Array<char> := ['a', 'b', 'c']; "
    "    var s: string := string.From(b, 0, 3); "
    "    total := total + s.Length; "
    "  end; "
    "  Console.WriteLine(total); "
    "  return 0; "
    "end Main; end C;";
constexpr const char* kFromValue =
    "module M; class C public static method Main(): int "
    "  var b: Array<char> := ['a', 'b', 'c']; "
    "  var s: string := string.From(b, 0, 3); "
    "  Console.WriteLine(s); "
    "  return 0; "
    "end Main; end C;";

// §9.6 `string.Trim(chars: IEnumerable<char>): string`.
constexpr const char* kTrimLoop =
    "module M; class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..<200 do "
    "    var cs: Array<char> := [' ']; "
    "    var s: string := \"abc\".Trim(cs); "
    "    total := total + s.Length; "
    "  end; "
    "  Console.WriteLine(total); "
    "  return 0; "
    "end Main; end C;";
constexpr const char* kTrimValue =
    "module M; class C public static method Main(): int "
    "  var cs: Array<char> := [' ']; "
    "  var s: string := \"abc\".Trim(cs); "
    "  Console.WriteLine(s); "
    "  return 0; "
    "end Main; end C;";

// ── THE CONTROLS (must stay CLEAN) ─────────────────────────────────────────

// CONTROL 1 (m1): the array alone, never passed ⇒ released (the pre-existing
// behaviour the fix must not disturb).
constexpr const char* kAloneLoop =
    "module M; class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..<200 do "
    "    var b: Array<char> := ['a', 'b', 'c']; "
    "    total := total + b.Count; "
    "  end; "
    "  Console.WriteLine(total); "
    "  return 0; "
    "end Main; end C;";

// CONTROL 2 (c2): the array passed to a USER method with a CONCRETE
// `Array<char>` formal — the #1430 `ms` relaxation's path. Clean before the
// fix and clean after it.
constexpr const char* kUserConcreteLoop =
    "module M; class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..<200 do "
    "    var b: Array<char> := ['a', 'b', 'c']; "
    "    total := total + Take(b); "
    "  end; "
    "  Console.WriteLine(total); "
    "  return 0; "
    "end Main; "
    "  private static method Take(x: Array<char>): int return x.Count; end Take; "
    "end C;";

// CONTROL 3 (c5): the array passed to a USER method with an `IEnumerable<char>`
// (INTERFACE) formal — the other `ms` shape. Clean before and after.
constexpr const char* kUserInterfaceLoop =
    "module M; class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..<200 do "
    "    var b: Array<char> := ['a', 'b', 'c']; "
    "    total := total + Take(b); "
    "  end; "
    "  Console.WriteLine(total); "
    "  return 0; "
    "end Main; "
    "  private static method Take(x: IEnumerable<char>): int return 1; end Take; "
    "end C;";

// CONTROL 4 (m4): the array passed to a user method that INTERNALLY calls
// `string.Join` — the leak must not reappear one level down, where the
// callee's own local is the parameter (not a local at all).
constexpr const char* kUserCallsJoinLoop =
    "module M; class C public static method Main(): int "
    "  var total: int := 0; "
    "  for var i in 0..<200 do "
    "    var a: Array<string> := [\"x\", \"y\"]; "
    "    total := total + Take(a).Length; "
    "  end; "
    "  Console.WriteLine(total); "
    "  return 0; "
    "end Main; "
    "  private static method Take(x: Array<string>): string "
    "    return string.Join(\",\", x); "
    "  end Take; "
    "end C;";

// CONTROL 5: the LOCAL IS NOT CONSUMED by the builtin call — the smoking
// gun. `a.Count` still reads the array AFTER `string.Join(",", a)` and gets
// the right value, so the scope-end release was suppressed WITHOUT an
// ownership transfer (a leak, not a move).
constexpr const char* kNotConsumed =
    "module M; class C public static method Main(): int "
    "  var a: Array<string> := [\"x\", \"y\"]; "
    "  var s: string := string.Join(\",\", a); "
    "  Console.WriteLine(a.Count); "
    "  Console.WriteLine(s); "
    "  return 0; "
    "end Main; end C;";

// CONTROL 6: the DOUBLE-FREE control. The brief's shape — an `owned`
// collection-typed formal (`method Take(owned x: Array<char>)`) — is NOT
// CONSTRUCTIBLE, and the reason is the spec's own: a BORROWING collection
// local carries NO ownership obligation (§28.9's trivial-dispose: the
// elements are not disposed and the binding is not owned), so it cannot be
// moved into an `owned` formal. MEASURED on this tree: `Take(move a)` with
// `owned x: Array<char>` is REJECTED with "a borrow cannot be passed where
// an `owned` value is required" (§28.8), and `Take(owned a)` is rejected with
// "invalid use of 'owned' at argument position (§A.4, proposal 0090)".
//
// ⇒ the NEAREST CONSTRUCTIBLE shape is an `owned` formal of an OWNING
// collection (`OwningArray<Array<int>>`, whose element type is a class as
// stdlib §4.2 requires). That form DOES transfer: the caller's scope-end
// release must be suppressed and the callee must free it exactly once. This
// row pins that there is no double free and no leak — a double free here
// would be a SEGV / ASan error, which is WORSE than the leak this fix
// removes.
constexpr const char* kOwnedFormalTransfer =
    "module M; class C public static method Main(): int "
    "  var a: OwningArray<Array<int>> := new OwningArray<Array<int>>(); "
    "  Console.WriteLine(Take(move a)); "
    "  return 0; "
    "end Main; "
    "  private static method Take(owned x: OwningArray<Array<int>>): int "
    "    return x.Count; "
    "  end Take; "
    "end C;";

// CONTROL 7: the CONTAINER-INSERT shape — a collection member that STORES its
// argument into an element slot. This is the shape the fix's relaxation must
// NOT touch: freeing the array at its scope end there would turn the
// pre-existing leak into a heap-use-after-free.
//
// ⚠ THIS PROGRAM STILL LEAKS (144 B / 2 allocations) — a PRE-EXISTING,
// SEPARATE defect: the borrow escapes the inner scope into the outer
// container and the compiler does not reject it (the ownership layer's
// borrow-escape check has no arm for a builtin container insert). The row
// pins the ABSENCE OF A MEMORY ERROR only, and runs with LeakSanitizer OFF
// (`runAotCheckedLenient`) so the known leak does not fail it; a
// use-after-free would still be caught by AddressSanitizer. The leak itself
// is REPORTED to the coordinator, not fixed here.
constexpr const char* kContainerInsert =
    "module M; class C public static method Main(): int "
    "  var l: Array<Array<int>> := []; "
    "  do "
    "    var a: Array<int> := [1, 2]; "
    "    l.Add(a); "
    "  end do; "
    "  Console.WriteLine(l[0][0]); "
    "  return 0; "
    "end Main; end C;";

}  // namespace

// ══ THE REPRO ROWS ═════════════════════════════════════════════════════════

// #7127 / #6889: the `items` argument of `string.Join` (stdlib §9.6) is a
// plain (borrowed) parameter, so the CALLER keeps the obligation and the
// array's scope-end free (§28.9 / stdlib §4.4) must run. Pre-fix this row
// fails with 200 × 208 B leaked.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           string_join_items_array_is_released_at_scope_end) {
    auto r = runAotChecked(kJoinLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

// #6889: the SAME site, reached through `Console.WriteLine` — the issue body
// called this "a different parameter type, not the same site"; it is the same
// site (MEASURED: the identical 208 B / 2 allocations).
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           console_write_line_array_is_released_at_scope_end) {
    auto r = runAotChecked(kConsoleLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    // The loop prints the array's own rendering on every iteration, so the
    // expected stdout is that rendering 200 times followed by the total.
    std::string expected;
    for (int i = 0; i < 200; ++i)
        expected += "System.Collections.Generic.Array<string>\n";
    expected += "400\n";
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

// §9.6 `string.From(buffer: Array<char>, start, length)` — the `buffer`
// parameter is a plain (borrowed) parameter.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           string_from_buffer_array_is_released_at_scope_end) {
    auto r = runAotChecked(kFromLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

// §9.6 `string.Trim(chars: IEnumerable<char>)` — the `chars` parameter is a
// plain (borrowed) parameter.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           string_trim_chars_array_is_released_at_scope_end) {
    auto r = runAotChecked(kTrimLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

// ══ THE VALUE ROWS (the printed output must not change) ═════════════════════

// The leak fix must not change WHAT the program computes. `string.Join(",",
// a)` still prints "x,y" — the value, not the exit code (a process exit
// status is masked to 0..255 and cannot carry a string).
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           string_join_items_value_is_unchanged) {
    auto r = runAotChecked(kJoinValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "x,y\n");
}

// `Console.WriteLine(a)` still prints the array's own rendering.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           console_write_line_array_value_is_unchanged) {
    auto r = runAotChecked(kConsoleValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "System.Collections.Generic.Array<string>\n");
}

// `string.From(b, 0, 3)` still prints "abc".
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           string_from_buffer_value_is_unchanged) {
    auto r = runAotChecked(kFromValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "abc\n");
}

// `"abc".Trim(cs)` still prints "abc".
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           string_trim_chars_value_is_unchanged) {
    auto r = runAotChecked(kTrimValue);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "abc\n");
}

// ══ THE CONTROLS ═══════════════════════════════════════════════════════════

// CONTROL 1 (m1): the array alone, never passed ⇒ released. This row is the
// pre-existing behaviour the fix must not disturb, and the control that says
// the relaxation is not simply "every array is released now".
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           array_alone_is_released_at_scope_end) {
    auto r = runAotChecked(kAloneLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

// CONTROL 2 (c2): the array passed to a USER method with a CONCRETE
// `Array<char>` formal. Clean before the fix (the #1430 `ms` relaxation
// already admitted it) and clean after it — the fix must not disturb the
// user-method path.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           user_method_concrete_parameter_keeps_the_array_released) {
    auto r = runAotChecked(kUserConcreteLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

// CONTROL 3 (c5): the array passed to a USER method with an `IEnumerable<char>`
// (INTERFACE) formal — the other `ms` shape.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           user_method_interface_parameter_keeps_the_array_released) {
    auto r = runAotChecked(kUserInterfaceLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// CONTROL 4 (m4): the array passed to a user method that INTERNALLY calls
// `string.Join` — the leak must not reappear one level down.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           user_method_calling_string_join_keeps_the_array_released) {
    auto r = runAotChecked(kUserCallsJoinLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "600\n");
}

// CONTROL 5: the local is NOT CONSUMED — `a.Count` still reads the array
// after the builtin call. This is the row that distinguishes "the scope-end
// release was suppressed" (the defect) from "the call consumed the array"
// (a transfer): the value is still live AND still correct, so nothing moved.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           builtin_call_does_not_consume_the_local) {
    auto r = runAotChecked(kNotConsumed);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "2\nx,y\n");
}

// CONTROL 6: the DOUBLE-FREE control. An `owned` formal TRANSFERS: the
// caller's scope-end release is suppressed and the callee frees it exactly
// once. The brief's `owned x: Array<char>` shape is not constructible (a
// borrowing collection local carries no ownership obligation — §28.9's
// trivial-dispose — so it cannot be moved into an `owned` formal; the
// compiler rejects it with "a borrow cannot be passed where an `owned` value
// is required"); the nearest constructible shape is the `OwningArray` one
// below. A double free here is a SEGV / ASan error — strictly worse than the
// leak this fix removes.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           owned_collection_formal_frees_exactly_once) {
    auto r = runAotChecked(kOwnedFormalTransfer);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// CONTROL 7: the CONTAINER-INSERT shape must NOT have become a
// use-after-free. The program still leaks (a pre-existing, separate defect —
// reported to the coordinator), so it runs with LeakSanitizer OFF; a
// use-after-free would still be caught by AddressSanitizer.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           container_insert_shape_is_not_freed_early) {
    auto r = runAotCheckedLenient(kContainerInsert);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(r.stdoutText, "1\n");
}

// ══ THE LEAK COUNTER (a number, not a verdict) ══════════════════════════════

// The harness's report-mode leak census must NOT move for the repro: a
// literal "the leak counter is 0" for this row, so the row cannot be
// satisfied by an oracle that is not armed. (Report mode counts without
// changing the verdict; `runAotChecked`'s strict net is the row above.)
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           leak_counter_stays_at_zero_for_the_join_repro) {
    LeakEnv env("report");
    const std::size_t before = leakCount();
    auto r = runAot(kJoinLoop);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_EQ(leakCount(), before);
}

// The control for that row: a program that DOES leak must move the counter,
// so the row above cannot be satisfied by a counter that never ticks.
BEZEL_TEST(builtin_collection_borrow_arg_leak_7127,
           leak_counter_moves_for_a_program_that_leaks) {
    LeakEnv env("report");
    const std::size_t before = leakCount();
    // The container-insert shape (CONTROL 7): MEASURED to leak 144 B / 2
    // allocations on this tree — a PRE-EXISTING, SEPARATE defect, used here
    // only as the oracle's own positive control.
    auto r = runAot(kContainerInsert);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(leakCount(), before + 1);
}
