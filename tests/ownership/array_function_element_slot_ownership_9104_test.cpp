// Ownership / disposal — a borrowing `Array<Fn>`'s element slot took NO
// reference of its own and gave NONE back: the collection ALIASED the closure
// cell it stored, so a fresh producer's reference was stranded (the closure
// env leaked per `Add`) and the cell's real owner freed it while the
// collection still held the pointer (a use-after-free). The slot's take must
// also recognise the PRODUCER THAT IS A CALL — a user method whose declared
// return type is a function type mints +1 for its caller — so the slot takes
// that single reference over instead of retaining on top of it (which
// stranded it). Issue #9104.
//
// Spec: §28.11 "Collection element ownership" *The element slot policy* (a
// FUNCTION type is in the *runtime-managed reference* row — with `string` and
// `Type` — whose borrowing-family column reads "shared references": the
// collection owns a reference of its OWN on the element it stores, and the
// operations table gives that reference back when the slot goes away —
// "Remove / RemoveAt: removes the reference", "Clear(): removes all", the
// collection's `Dispose()`: "no user-visible work", the references it holds
// go back invisibly), §28.9 "Runtime-managed types" (a function value's
// lifetime ends when the last reference to it goes), §27.5 (a closure's
// captures stay alive as long as the closure). Spec revision v0.359.
//
// Model: the `string` row of the same table, which the compiler already
// implements in full (`arrIsStringElem` / `stringElementReleaseThunk`).
//
// Every closure is built at run time (a capture-free lambda still allocates
// its env), and every row loops 200 times so that LSan sees the leak.
// runAotChecked runs under ASan/LSan, so a leak (the bug), a double-free and
// a use-after-free all flip `r.ran`; stdout pins the values.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// The issue's DECISIVE repro (its Probe A): the closure is created FRESH at
// the `Add` site, so the array is its only holder — "the producer's reference
// is the array's, and the array never gives it back". 200 iterations, each
// adding one closure: 200 leaked envs on the base. The program prints 600.
const char* kAddFreshProducer =
    "module M;\n"
    "type Fn := (int, int) -> int;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<Fn> := new Array<Fn>();\n"
    "      fs.Add(lambda(x: int, y: int) => x + y);\n"
    "      for var g in fs do total := total + g(1, 2); end for;\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The removal half of the pair: the slot's reference is given back when the
// slot goes away ("Remove / RemoveAt: removes the reference"). On the base
// the `Clear` released nothing, so the stored closure's env leaked.
const char* kAddFreshProducerThenClear =
    "module M;\n"
    "type Fn := (int, int) -> int;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<Fn> := new Array<Fn>();\n"
    "      fs.Add(lambda(x: int, y: int) => x + y);\n"
    "      fs.Clear();\n"
    "      total := total + fs.Count;\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The OVERWRITE half: an indexer-set "stores the reference" on the new value
// and "removes the reference" on the DISPLACED one. On the base both the
// displaced closure and the stored one leaked (the setter had no disposer and
// took nothing).
const char* kIndexerSetDisplaces =
    "module M;\n"
    "type Fn := (int, int) -> int;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<Fn> := new Array<Fn>();\n"
    "      fs.Add(lambda(x: int, y: int) => x + y);\n"
    "      fs[0] := lambda(x: int, y: int) => x + y + 1;\n"
    "      for var g in fs do total := total + g(1, 2); end for;\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The METHOD-GROUP shape (the issue's Probe D): the stored value is not a
// lambda but a static method reference — "it is not about the lambda's env
// specifically: the collection's reference to a function-typed element is
// dropped by nobody".
const char* kAddMethodGroup =
    "module M;\n"
    "type Fn := (int, int) -> int;\n"
    "public class C\n"
    "  public static method Add1(x: int, y: int): int\n"
    "    return x + y;\n"
    "  end Add1;\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<Fn> := new Array<Fn>();\n"
    "      fs.Add(Add1);\n"
    "      for var g in fs do total := total + g(1, 2); end for;\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: the `string` twin of the same row is unchanged — it already took
// and released the +1 per element (the row this fix mirrors).
const char* kControlStringElement =
    "module M;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<string> := new Array<string>();\n"
    "      fs.Add(\"ab\" + \"cd\");\n"
    "      for var g in fs do total := total + g.Length; end for;\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// Control: an element type with no runtime-managed reference is unchanged —
// the slot copies the value and owns nothing.
const char* kControlValueElement =
    "module M;\n"
    "public class C\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<int> := new Array<int>();\n"
    "      fs.Add(i);\n"
    "      for var g in fs do total := total + g; end for;\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The PRODUCER that is a CALL: a user method whose declared return type is a
// function type mints +1 for its caller (§28.9 / #712), so the slot must TAKE
// that single reference over (the `string` row's rule) instead of retaining on
// top of it. On the base the slot retained, the producer's own +1 was
// stranded, and the closure's env leaked (LSan: 15 200 B / 600 allocations
// over 200 iterations). The indexer-set also gives the DISPLACED element's
// reference back ("removes the reference").
const char* kIndexerSetClosureReturningCall =
    "module M;\n"
    "public class C\n"
    "  public static method Make(): () -> int\n"
    "    return lambda() => 7;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<() -> int> := new Array<() -> int>();\n"
    "      fs.Add(lambda() => 3);\n"
    "      fs[0] := Make();\n"
    "      for var g in fs do total := total + g(); end for;\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// The FILL constructor's twin: `new Array<Fn>(n, v)` fills every slot with
// the SAME value, so every slot takes a reference of its own and the
// producer's own reference is surplus — it must be given back. On the base the
// surplus was stranded (the same LSan shape as the row above).
const char* kFillCtorClosureReturningCall =
    "module M;\n"
    "public class C\n"
    "  public static method Make(): () -> int\n"
    "    return lambda() => 7;\n"
    "  end Make;\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs := new Array<() -> int>(3, Make());\n"
    "      for var g in fs do total := total + g(); end for;\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// fix-forward #9104 (review finding 1): the SAME `Array<Fn>` reached through
// stdlib §4.4's `IList<T>` interface (`Array<T>: IList<T>`). The slot's take
// fired only on the static `Array` spelling while the give-back fired on every
// `Array<Fn>`, so an `Add` through an `IList<() -> int>` parameter stored a
// borrowed closure WITHOUT a take, and the array's disposal then released the
// holder's own reference: a use-after-free at the next read of `keep.cb`.
const char* kIListAddBorrowed =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Program\n"
    "  public static method Fill(xs: IList<() -> int>, h: H)\n"
    "    xs.Add(h.cb);\n"
    "  end Fill;\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<() -> int> := new Array<() -> int>();\n"
    "      Fill(fs, keep);\n"
    "      total := total + fs[0]();\n"
    "    end for;\n"
    "    total := total + keep.cb();\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The indexer-set twin through `IList<T>`: the store must take the new
// element's reference (a borrowed one and a fresh one) and give back the one
// it displaces, as the static `Array` spelling does.
const char* kIListIndexerSet =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Program\n"
    "  public static method Put(xs: IList<() -> int>, h: H)\n"
    "    xs[0] := h.cb;\n"
    "    xs[1] := lambda() => 4;\n"
    "  end Put;\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<() -> int> := new Array<() -> int>();\n"
    "      fs.Add(lambda() => 9);\n"
    "      fs.Add(lambda() => 9);\n"
    "      Put(fs, keep);\n"
    "      total := total + fs[0]() + fs[1]();\n"
    "    end for;\n"
    "    total := total + keep.cb();\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// fix-forward #9104 (review finding 1): `FrozenArray<T>.From(items)` (stdlib
// §4.19) COPIES the array's slots. The copy's free gave the element reference
// back while the copy never took one, so a `Clear()` of the source freed the
// closure the frozen copy still read: a use-after-free at `z[0]()`.
const char* kFrozenFromThenClear =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var fs: Array<() -> int> := new Array<() -> int>();\n"
    "      fs.Add(lambda() => 2);\n"
    "      using var z := FrozenArray<() -> int>.From(fs);\n"
    "      fs.Clear();\n"
    "      total := total + z[0]();\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

void expectClean(const char* src, const char* out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak (the bug), no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

}  // namespace

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           add_producer_element_reference_is_given_back_spec_28_11) {
    expectClean(kAddFreshProducer, "600\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           clear_returns_the_element_reference_spec_28_11) {
    expectClean(kAddFreshProducerThenClear, "0\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           indexer_set_returns_the_displaced_reference_spec_28_11) {
    expectClean(kIndexerSetDisplaces, "800\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           add_method_group_element_reference_is_given_back_spec_28_11) {
    expectClean(kAddMethodGroup, "600\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           control_string_element_array_unchanged_spec_28_11) {
    expectClean(kControlStringElement, "800\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           control_value_element_array_unchanged_spec_28_11) {
    expectClean(kControlValueElement, "19900\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           indexer_set_of_a_closure_returning_call_takes_its_reference_spec_28_9) {
    expectClean(kIndexerSetClosureReturningCall, "1400\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           fill_constructor_of_a_closure_returning_call_takes_every_slot_spec_28_11) {
    expectClean(kFillCtorClosureReturningCall, "4200\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           ilist_add_of_a_borrowed_closure_takes_its_reference_spec_28_11) {
    expectClean(kIListAddBorrowed, "603\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           ilist_indexer_set_takes_and_gives_back_spec_28_11) {
    expectClean(kIListIndexerSet, "1403\n");
}

BEZEL_TEST(ownership_array_function_element_slot_ownership_9104,
           frozen_array_from_copy_takes_its_own_references_spec_28_11) {
    expectClean(kFrozenFromThenClear, "400\n");
}
