// Ownership / disposal — a closure held in GENERIC `T` storage was
// mis-co-owned: the generic field's store took NO reference of its own while
// the generic field's disposal released it, so the same closure reference was
// released twice (the runtime's fatal over-release, exit 134) or the holder's
// reference was stranded (the closure's env leaked). A built-in collection's
// ELEMENT READ (`Stack<T>.Peek()`) was also treated as a +1 producer, so the
// chain-call `st.Peek()()` released the collection's own reference and freed
// the closure while the caller was still calling it (a heap-use-after-free).
// And `new Stack<T>(arr)` — a constructor the stdlib does not declare — fell
// into the capacity path and emitted malformed IR ("IR verification failed").
// Issue #9112.
//
// Spec: §28.11 "Collection element ownership" *The element slot policy* (the
// *runtime-managed reference* row — a FUNCTION type, with `string` and `Type`
// — is a shared-reference slot: the holder owns a reference of its OWN on the
// value it stores, and the read hands out "reference out"), §28.9
// "Runtime-managed types" (two releases for one reference is a fatal
// over-release), §28.3's producers table (a BARE type parameter is a BORROW,
// not a minting return type), stdlib §4.11 (`Stack<T>` / `Queue<T>` declare
// only the no-argument constructor; `Peek` is a "borrow of top element"), and
// §14.1 / §3.6 rule 2 (the element category — and the co-ownership rule — is
// fixed at the INSTANTIATION, so a generic field's co-ownership is decided on
// the SUBSTITUTED type). Spec revision v0.359.
//
// Model: the `string` row of the same table, which the compiler implements in
// full, and the `Array<Fn>` slot row of issue #9104.
//
// Every row loops 200 times so that LSan sees the leak and the runtime's
// over-release guard fires; runAotChecked runs under ASan/LSan, so a leak, an
// over-release abort and a use-after-free all flip `r.ran`; stdout pins the
// values.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// The generic-`T` FIELD half: a class whose field is declared at the type
// parameter `T`, instantiated at a function type. The constructor stores the
// argument into the field; `Pop` reads the field back out. The field's own
// reference and the holder's reference must be one reference, released once —
// on the base the field's store took NOTHING, so the field's disposal released
// a reference the field never took: the runtime's fatal over-release (exit
// 134, "refcount over-release (closure)").
const char* kGenericTFieldPop =
    "module P;\n"
    "import System;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Box<T>\n"
    "  public item: T;\n"
    "  public constructor(x: T)\n"
    "    item := x;\n"
    "  end constructor;\n"
    "  public method Pop(): T\n"
    "    return item;\n"
    "  end Pop;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var acc := 0;\n"
    "    var keep := new H();\n"
    "    var b := new Box<() -> int>(keep.cb);\n"
    "    for var i in 0..<200 do\n"
    "      var g := b.Pop();\n"
    "      acc := acc + g();\n"
    "    end;\n"
    "    Console.WriteLine(acc);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The generic-`T` field read into a local that is then called: the reader
// must hold the field's reference (the field keeps its own), so the closure
// is alive for the call and released exactly once per holder.
const char* kGenericTFieldRead =
    "module P;\n"
    "import System;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Box<T>\n"
    "  public item: T;\n"
    "  public constructor(x: T)\n"
    "    item := x;\n"
    "  end constructor;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var acc := 0;\n"
    "    var keep := new H();\n"
    "    var b := new Box<() -> int>(keep.cb);\n"
    "    for var i in 0..<200 do\n"
    "      var g := b.item;\n"
    "      acc := acc + g();\n"
    "    end;\n"
    "    Console.WriteLine(acc);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The generic `T` storage in a BUILT-IN collection: an `owned Array<T>` field
// of a generic class, instantiated at a function type. The store into the
// array's element slot takes a reference of the collection's own (§28.11's
// shared-reference row) and the collection's disposal gives it back.
const char* kGenericTCollectionField =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Bag<T>\n"
    "  private owned items: Array<T> := new Array<T>();\n"
    "  public method Put(x: T)\n"
    "    items.Add(x);\n"
    "  end Put;\n"
    "end Bag;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var keep := new H();\n"
    "    var b := new Bag<() -> int>();\n"
    "    b.Put(keep.cb);\n"
    "    Console.WriteLine(keep.cb());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The STACK half: a function-typed element read back out of a built-in
// `Stack` and invoked at the call site. On the base the chain-call release
// released the peeked closure before the call read it: a heap-use-after-free.
const char* kStackPeekInvoke =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var acc := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      using var st: Stack<() -> int> := new Stack<() -> int>();\n"
    "      st.Push(keep.cb);\n"
    "      acc := acc + st.Peek()();\n"
    "    end for;\n"
    "    Console.WriteLine(acc);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The CONSTRUCTOR half: `Stack<T>` declares only the no-argument constructor
// (stdlib §4.11), so `new Stack<() -> int>(arr)` has no constructor to bind
// to. On the base the argument fell into the array family's capacity path —
// a pointer where an `int` capacity was expected — and the compiler emitted
// malformed IR ("Invalid operands for select instruction! ... ptr ..., i64 1")
// and died with "IR verification failed" instead of a diagnostic.
const char* kStackCtorWithArgument =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var arr: Array<() -> int> := new Array<() -> int>();\n"
    "    arr.Add(lambda() => 7);\n"
    "    using var st := new Stack<() -> int>(arr);\n"
    "    Console.WriteLine(st.Peek()());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// fix-forward #9112 (review finding 2): `Pop` REMOVES the element, so the
// chain call's release is the only discharge of a freshly pushed lambda's
// reference (the borrowing `Stack<T>` takes none of its own at `Push`, #9172).
// The parent fix dropped that release with `Peek`'s: a leak per iteration.
const char* kStackPopFresh =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      using var st: Stack<() -> int> := new Stack<() -> int>();\n"
    "      st.Push(lambda() => 2);\n"
    "      total := total + st.Pop()();\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The `Queue` twin: `Dequeue` removes the element too.
const char* kQueueDequeueFresh =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      using var q: Queue<() -> int> := new Queue<() -> int>();\n"
    "      q.Enqueue(lambda() => 2);\n"
    "      total := total + q.Dequeue()();\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// fix-forward #9112 (review finding 3): the constructor's generic-`T` store
// TAKES a reference; a later store into the same field (here from outside the
// generic body) must give the displaced one back. The local `f` keeps its own
// reference and releases it at scope end.
const char* kBoxFieldReassignOutside =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Box<T>\n"
    "  public item: T;\n"
    "  public constructor(x: T)\n"
    "    item := x;\n"
    "  end constructor;\n"
    "  public method Set(x: T)\n"
    "    item := x;\n"
    "  end Set;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      var f: () -> int := lambda() => 2;\n"
    "      using var b := new Box<() -> int>(f);\n"
    "      b.item := keep.cb;\n"
    "      total := total + b.item();\n"
    "    end for;\n"
    "    total := total + keep.cb();\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// A lambda LITERAL passed to the constructor's `T` formal: the store takes
// the field's reference, so the caller's temporary must still be released.
const char* kBoxCtorLiteral =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Box<T>\n"
    "  public item: T;\n"
    "  public constructor(x: T)\n"
    "    item := x;\n"
    "  end constructor;\n"
    "  public method Set(x: T)\n"
    "    item := x;\n"
    "  end Set;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      using var b := new Box<() -> int>(lambda() => 2);\n"
    "      total := total + b.item();\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// fix-forward #9112: `b.Get()()` — a user method whose declared return is
// the bare `T` hands out a BORROW of the field (its `return item` takes no
// reference), so the chain call must not release it the way it releases a
// +1 producer. With a BORROWED constructor argument the parent's release
// dropped the field's own reference, and the next read `b.item()` was a
// use-after-free.
const char* kBoxGetChainCall =
    "module P;\n"
    "import System;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Box<T>\n"
    "  public item: T;\n"
    "  public constructor(x: T)\n"
    "    item := x;\n"
    "  end constructor;\n"
    "  public method Get(): T\n"
    "    return item;\n"
    "  end Get;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      using var b := new Box<() -> int>(keep.cb);\n"
    "      total := total + b.Get()() + b.item();\n"
    "    end for;\n"
    "    total := total + keep.cb();\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// fix-forward #9112: a generic class's `Array<T>` FIELD at `T = () -> int`.
// The element store TAKES the slot's reference (§28.11), so a lambda literal
// passed to `Put(x: T)` is still the caller's to release, and the local `f`
// keeps its own scope-end release; `At(i)()` borrows the element.
const char* kGenericBagArrayField =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Bag<T>\n"
    "  private owned items: Array<T> := new Array<T>();\n"
    "  public method Put(x: T)\n"
    "    items.Add(x);\n"
    "  end Put;\n"
    "  public method At(i: int): T\n"
    "    return items[i];\n"
    "  end At;\n"
    "  public method Drop()\n"
    "    items.RemoveAt(0);\n"
    "  end Drop;\n"
    "end Bag;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    for var i in 0..<200 do\n"
    "      using var b := new Bag<() -> int>();\n"
    "      var f: () -> int := lambda() => 2;\n"
    "      b.Put(f); b.Put(keep.cb); b.Put(lambda() => 4);\n"
    "      var g := b.At(0);\n"
    "      total := total + g() + b.At(1)() + b.At(2)();\n"
    "      b.Drop();\n"
    "    end for;\n"
    "    total := total + keep.cb();\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

void expectClean(const char* src, const char* out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak, no over-release, no UAF
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

// fix-forward #9112 (§28.9 / §9.7, v0.359): a method whose declared return
// is the bare `T` hands its caller the field's pointer with NO reference of
// its own (#9239), so a generic-`T` field store must NOT release the value it
// displaces: `var r := b.Get(); b.Set(lambda() => 7); r()` would call a freed
// closure. §28.9 makes the function value's lifetime the runtime's, so `r`
// stays callable. Review r43-rev-c44-9104-ff's r01e (straight-line).
const char* kGetAliasThenSet =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Box<T>\n"
    "  public item: T;\n"
    "  public constructor(x: T)\n"
    "    item := x;\n"
    "  end constructor;\n"
    "  public method Get(): T\n"
    "    return item;\n"
    "  end Get;\n"
    "  public method Set(x: T)\n"
    "    item := x;\n"
    "  end Set;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    var keep := new H();\n"
    "    using var b := new Box<() -> int>(lambda() => 2);\n"
    "    var r := b.Get();\n"
    "    b.Set(lambda() => 7);\n"
    "    total := total + r() + b.item();\n"
    "    total := total + keep.cb();\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// The same alias held in ANOTHER generic function (`Swap2<T>`), where no
// guard local to the storing method could see it. Review probe r03.
const char* kGetAliasInOtherGenericFunction =
    "module P;\n"
    "import System;\n"
    "import System.Collections;\n"
    "public class H\n"
    "  public cb: () -> int := lambda() => 3;\n"
    "end H;\n"
    "public class Box<T>\n"
    "  public item: T;\n"
    "  public constructor(x: T)\n"
    "    item := x;\n"
    "  end constructor;\n"
    "  public method Get(): T\n"
    "    return item;\n"
    "  end Get;\n"
    "  public method Set(x: T)\n"
    "    item := x;\n"
    "  end Set;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method Swap2<T>(b: Box<T>, x: T): T\n"
    "    var old := b.Get();\n"
    "    b.Set(x);\n"
    "    return old;\n"
    "  end Swap2;\n"
    "  public static method Main(): int\n"
    "    var total := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var b := new Box<() -> int>(lambda() => 2);\n"
    "      var o := Swap2<() -> int>(b, lambda() => 9);\n"
    "      total := total + o() + b.item();\n"
    "    end for;\n"
    "    Console.WriteLine(total);\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";

// Memory-safety oracle only: ASan stays armed (a use-after-free or double
// free fails the row) but LeakSanitizer is off — the displaced value is kept
// on purpose (a leak, tracked under #9239), so these rows pin the ABSENCE of
// the use-after-free and the program's output.
void expectSafe(const char* src, const char* out) {
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string(out));
}

// True iff SOME diagnostic names the constructor shape the stdlib declares
// for the LIFO/FIFO families (stdlib §4.11) and cites the issue.
bool hasNoArgumentCtorDiagnostic(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("takes no arguments") != std::string::npos &&
            d.message.find("\xC2\xA7""4.11") != std::string::npos &&
            d.message.find("#9112") != std::string::npos)
            return true;
    return false;
}

}  // namespace

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           generic_t_field_pop_hands_out_a_reference_spec_28_11) {
    expectClean(kGenericTFieldPop, "600\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           generic_t_field_read_hands_out_a_reference_spec_28_11) {
    expectClean(kGenericTFieldRead, "600\n");
}

// CONTROL (green on staging as well; one of this suite's at most two named
// controls): a builtin collection held in a generic `T` field keeps its slot's
// own reference.
BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           generic_t_collection_field_slot_is_co_owned_spec_28_11) {
    expectClean(kGenericTCollectionField, "3\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           stack_peek_invoke_does_not_release_the_element_spec_28_9) {
    expectClean(kStackPeekInvoke, "600\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           stack_constructor_with_an_argument_is_a_diagnostic_spec_4_11) {
    JitResult r = runAot(kStackCtorWithArgument);
    BEZEL_EXPECT_FALSE(r.ran);              // rejected, not compiled
    BEZEL_EXPECT_TRUE(hasNoArgumentCtorDiagnostic(r));
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           stack_pop_invoke_releases_the_removed_element_spec_28_9) {
    expectClean(kStackPopFresh, "400\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           queue_dequeue_invoke_releases_the_removed_element_spec_28_9) {
    expectClean(kQueueDequeueFresh, "400\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           generic_t_field_reassign_gives_back_the_displaced_reference_spec_28_11) {
    expectClean(kBoxFieldReassignOutside, "603\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           generic_t_constructor_store_of_a_literal_balances_spec_28_11) {
    expectClean(kBoxCtorLiteral, "400\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           type_param_return_chain_call_borrows_the_field_spec_28_9) {
    expectClean(kBoxGetChainCall, "1203\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           generic_array_t_field_element_store_balances_spec_28_11) {
    expectClean(kGenericBagArrayField, "1803\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           get_alias_survives_a_setter_store_spec_28_9) {
    expectSafe(kGetAliasThenSet, "12\n");
}

BEZEL_TEST(ownership_generic_t_closure_field_ownership_9112,
           get_alias_in_another_generic_function_survives_a_setter_store_spec_28_9) {
    expectSafe(kGetAliasInOtherGenericFunction, "2200\n");
}
