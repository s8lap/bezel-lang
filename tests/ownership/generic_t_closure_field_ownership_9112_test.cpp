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

void expectClean(const char* src, const char* out) {
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);          // no leak, no over-release, no UAF
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
