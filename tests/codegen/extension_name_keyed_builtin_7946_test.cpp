// Regression tests for #7946 -- a name-keyed built-in collection arm (Peek /
// Pop / Push / Enqueue / ...) in Emitter::tryLowerCollectionInstanceCall
// answered a call sema had bound to a user extension on a receiver whose head
// does not declare the name (a user `extension Array<int>` Peek(n) called as
// `a.Peek(10)` printed 0, not 7).
//
// WHAT THE SPEC REQUIRES.  Language spec §26.5 rules 1-2 (v0.350): extension
// blocks are consulted only for a name the receiver's declared surface does
// not declare (stdlib §4.4-§4.11 list each head's members); §22.6 binds the
// call to the selected candidate.
//
// ORACLE: exact stdout.

#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotOut;

namespace {
std::string prog(const std::string& ext, const std::string& decl,
                 const std::string& body) {
    return "module R; " + ext +
           " class Program public static method Main(): int " + decl + body +
           " return 0; end Main; end Program;";
}
}  // namespace

// §26.5 rule 2 (v0.350): Array declares no `Peek`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_peek) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Peek(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.Peek(10));")), "7");
}

// §26.5 rule 2 (v0.350): Array declares no `Pop`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_pop) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Pop(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.Pop(10));")), "7");
}

// §26.5 rule 2 (v0.350): Array declares no `Push`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_push) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Push(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.Push(10));")), "7");
}

// §26.5 rule 2 (v0.350): Array declares no `Enqueue`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_enqueue) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Enqueue(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.Enqueue(10));")), "7");
}

// §26.5 rule 2 (v0.350): Array declares no `Dequeue`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_dequeue) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Dequeue(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.Dequeue(10));")), "7");
}

// §26.5 rule 2 (v0.350): Array declares no `TryPeek`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_trypeek) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method TryPeek(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.TryPeek(10));")), "7");
}

// §26.5 rule 2 (v0.350): Array declares no `TryPop`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_trypop) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method TryPop(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.TryPop(10));")), "7");
}

// §26.5 rule 2 (v0.350): Array declares no `TryDequeue`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_trydequeue) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method TryDequeue(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.TryDequeue(10));")), "7");
}

// §26.5 rule 2 (v0.350): Array declares no `Drain`; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, array_drain) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Array<int> public method Drain(n: int): int => 7; end;",
        " var a: Array<int> := [1,2,3]; ", "Console.WriteLine(a.Drain(10));")), "7");
}

// §26.5 rule 2 (v0.350): Set declares no Peek; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, set_peek) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Set<int> public method Peek(n: int): int => 7; end;",
        " var a: Set<int> := new Set<int>(); ", "Console.WriteLine(a.Peek(10));")), "7");
}

// §26.5 rule 2 (v0.350): Dictionary declares no Pop; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, dictionary_pop) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Dictionary<int, string> public method Pop(n: int): int => 7; end;",
        " var a: Dictionary<int, string> := new Dictionary<int, string>(); ", "Console.WriteLine(a.Pop(10));")), "7");
}

// §26.5 rule 2 (v0.350): Queue declares no Push; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, queue_push) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Queue<int> public method Push(n: int): int => 7; end;",
        " var a: Queue<int> := new Queue<int>(); ", "Console.WriteLine(a.Push(10));")), "7");
}

// §26.5 rule 2 (v0.350): Stack declares no Enqueue; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, stack_enqueue) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Stack<int> public method Enqueue(n: int): int => 7; end;",
        " var a: Stack<int> := new Stack<int>(); ", "Console.WriteLine(a.Enqueue(10));")), "7");
}

// §26.5 rule 2 (v0.350): Stack declares no Dequeue; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, stack_dequeue) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Stack<int> public method Dequeue(n: int): int => 7; end;",
        " var a: Stack<int> := new Stack<int>(); ", "Console.WriteLine(a.Dequeue(10));")), "7");
}

// §26.5 rule 2 (v0.350): Queue declares no Pop; the extension is selected.
BEZEL_TEST(extension_name_keyed_builtin_7946, queue_pop) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Queue<int> public method Pop(n: int): int => 7; end;",
        " var a: Queue<int> := new Queue<int>(); ", "Console.WriteLine(a.Pop(10));")), "7");
}

// §26.5 rule 1 (v0.350), control: Stack declares Peek, so a declared name
// keeps the built-in even with a same-named extension present.
BEZEL_TEST(extension_name_keyed_builtin_7946, stack_declared_peek_keeps_builtin) {
    BEZEL_EXPECT_EQ(runAotOut(prog(
        "extension Stack<int> public method Peek(n: int): int => 7; end;",
        " var a: Stack<int> := new Stack<int>(); a.Push(5); ",
        "Console.WriteLine(a.Peek());")), "5");
}
