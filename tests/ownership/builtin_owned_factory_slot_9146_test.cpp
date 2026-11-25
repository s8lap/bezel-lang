// Ownership — issue #9146 (stdlib §4.14, stdlib §4.9, §27.1.1 fourth bullet,
// §28.3.5 rows 1 and 4, v0.359): a PLAIN function value is refused at a
// STDLIB built-in factory slot whose parameter type returns `owned K`.
//
// THE GOVERNING TEXT.
//
//   §27.1.1 (v0.359), fourth bullet: "Modifier slots (`owned`/`ref`/`out`/
//   `out owned`/`scoped`) must match exactly".
//   §28.3.5 (v0.359) row 1: a call through an `owned` slot is "Owned" — the
//   CALLER disposes each result. Row 4: a call through a plain concrete
//   class/interface slot is a "Borrow … The result is never owned".
//
//   stdlib §4.14 (`OwningBuffer<T>`, the CLASS-typed presentation):
//     public constructor(length: int, init: (int) -> owned T);
//   stdlib §4.9 (`OwningDictionary<K, V> where V: class`):
//     public method GetOrAdd(key: K, factory: (K) -> owned V): V;
//
// THE DEFECT. A built-in constructor or method has no `ast::Param` list, so
// its arguments never reach `checkAssignable` — the §27.1.1 modifier-slot
// question is asked NOWHERE on that path — and the ONE built-in parameter
// table's `kinds` column has no bit for a FUNCTION type, so the position
// carried no type opinion either. A plain function value (a `(..) -> K`
// variable, or a method group over a borrow-returning method) was therefore
// ADMITTED at `OwningBuffer<T>`'s `init` and at `OwningDictionary<K, V>`'s
// `factory`. Both callees TAKE the result's ownership and dispose it — the
// buffer's destructor cascades through its slots, the dictionary stores the
// value (taking ownership) — so the callee disposed a BORROW of storage
// someone else owns.
//
// MEASURED on r30-c1 (fb1de7ae6), under ASan (`--probe --asan`):
//   * `new OwningBuffer<K>(2, f)` with `f: (int) -> K` —
//     `AddressSanitizer: heap-use-after-free … in bz$elemDispose`
//     (`bz_array_free`, src/runtime/bezel_runtime_collections.cpp:1107);
//     the same report over a 200-iteration loop.
//   * `OwningDictionary<int, K>.GetOrAdd(1, f)` with `f: (int) -> K` —
//     `AddressSanitizer: heap-use-after-free … in main`.
//
// THE BORROWING `Dictionary<K, V>` IS NOT AFFECTED: stdlib §4.8 declares its
// factory as `(K) -> V` — NO `owned` marker — the dictionary owns nothing, and
// the stored value is a borrow, so a plain `(K) -> V` factory is CORRECT
// there (measured: no ASan report, exit 0). The admitting twin is in
// `corpus-candidates/`, not here.
//
// THE FIX. The built-in constructor/method arm now asks the SAME rule the
// user-declared arm asks (#9127): a value whose return slot is the plain
// concrete class/interface `K` is refused where the target's return slot is
// `owned`. The predicate and the sentence are ONE
// (`ExprTC::checkPlainFunctionValueAtOwnedSlot`), so the built-in arm and the
// user-declared arm cannot drift.
//
// ROWS. Two RED rows (each RED on r30-c1: the program is admitted and no
// diagnostic is reported) and two boundary controls, each GREEN on both arms
// and RED under a named wrong fix. All four rows run on the AOT oracle's
// CHECKED variant (`runAotChecked` — the checked oracle ownership work uses:
// it links the program under ASan/LSan and verifies dispose-exactly-once), so
// the two controls' disposal counts are the oracle's own verdict.
//
// per stdlib §4.9, stdlib §4.14, §27.1.1, §28.3.5 rows 1 and 4, v0.359

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAotChecked;

namespace {

// The class-typed element the buffer and the dictionary own.
const char* kKlass =
    "public class K "
    "  public readonly N: int; "
    "  public constructor(n: int) N := n; end constructor; "
    "  destructor K.disposed := K.disposed + 1; end destructor; "
    "  public static disposed: int := 0; "
    "end K; ";

// The issue's own repro: a plain `(int) -> K` value (a lambda that returns a
// BORROW of a `using var` local) at `OwningBuffer<K>`'s `init` slot.
std::string owningBufferPlainValue() {
    return std::string("module M; ") + kKlass +
           "public class Program "
           "  public static method Main(): int "
           "    using var k := new K(7); "
           "    var f: (int) -> K := lambda(i: int) => k; "
           "    using var b := new OwningBuffer<K>(2, f); "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

// The same shape at `OwningDictionary<K, V>`'s `factory` slot, amplified over
// 200 iterations so the defect is not a one-shot (CLAUDE.md's 50-500x rule
// for a memory repro).
std::string owningDictionaryPlainValue() {
    return std::string("module M; ") + kKlass +
           "public class Program "
           "  public static method Main(): int "
           "    var i := 0; "
           "    while i < 200 do "
           "      using var k := new K(7); "
           "      var f: (int) -> K := lambda(i: int) => k; "
           "      using var d := new OwningDictionary<int, K>(); "
           "      var v := d.GetOrAdd(i, f); "
           "      i := i + 1; "
           "    end; "
           "    return 0; "
           "  end Main; "
           "end Program;";
}

void expectRefusedAtOwnedSlot(const JitResult& r, const char* position) {
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, position));
    // The §9127 sentence: the SOURCE's return slot is the plain concrete
    // class/interface, the TARGET's is `owned`.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "but the target's is `owned`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "return slot is the plain"));
}

}  // namespace

// --- Row 1: the issue's own repro, at `OwningBuffer<T>`'s `init` ----------

BEZEL_TEST(ownership_builtin_owned_factory_slot_9146,
           owning_buffer_init_refuses_a_plain_function_value_9146) {
    expectRefusedAtOwnedSlot(runAotChecked(owningBufferPlainValue()),
                             "argument to `OwningBuffer` constructor");
}

// --- Row 2: the same shape at `OwningDictionary<K, V>`'s `factory` --------

BEZEL_TEST(ownership_builtin_owned_factory_slot_9146,
           owning_dictionary_get_or_add_refuses_a_plain_function_value_9146) {
    expectRefusedAtOwnedSlot(runAotChecked(owningDictionaryPlainValue()),
                             "argument to `OwningDictionary.GetOrAdd`");
}

// --- Control 1: a FRESH LAMBDA LITERAL at the `init` slot stays admitted ---
//
// The fresh lambda is the callee-side-checked spelling (#9040/#1820: its body
// must supply a fresh or owned value) and is skipped by the rule — the issue
// says so explicitly. Its body mints a fresh `K`, the buffer owns it and
// disposes it exactly once. RED under the wrong fix "refuse every function
// value at an `owned` slot, fresh lambdas included".

BEZEL_TEST(ownership_builtin_owned_factory_slot_9146,
           owning_buffer_init_still_admits_a_fresh_lambda_9146) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      using var b := new OwningBuffer<K>(1, lambda(j: int) => new K(7)); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(K.disposed); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}

// --- Control 2: an `owned`-TYPED function variable at the `factory` slot ---
//
// An `owned`-returning function value is not a plain value: the rule skips
// it, and the dictionary owns each result and disposes it exactly once. RED
// under the wrong fix "refuse every function value at an `owned` slot".

BEZEL_TEST(ownership_builtin_owned_factory_slot_9146,
           owning_dictionary_get_or_add_still_admits_an_owned_function_value_9146) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kKlass +
        "public class Program "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 200 do "
        "      var f: (int) -> owned K := lambda(j: int) => new K(7); "
        "      using var d := new OwningDictionary<int, K>(); "
        "      var v := d.GetOrAdd(i, f); "
        "      i := i + 1; "
        "    end; "
        "    Console.WriteLine(K.disposed); "
        "    return 0; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "200\n");
}
