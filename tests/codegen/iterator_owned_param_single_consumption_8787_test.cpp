// An iterator method that declares an `owned` parameter mints a
// single-consumption iterable (fix #8787).
//
// §29.8 (v0.359): "an iterable produced by an iterator method that declares an
// `owned` or move-only parameter ... is single-consumption, under exactly
// §29.11's rule for a lazy query — its enumeration right is spent at the first
// consumption point (a `for` or `await for`, a terminal operator, a
// `GetEnumerator()` or `GetAsyncEnumerator()` call or its owning form, a
// further deferred operator, or passing it as an `IEnumerable<T>`/
// `IAsyncEnumerable<T>` argument); a second consumption is a compile error
// where the flow analysis can see it and raises `InvalidOperationException`
// where it is reachable only at run time". And: "The first `GetEnumerator()`
// ... transfers the frame's `owned` and move-only parameters to the enumerator
// it produces; that enumerator disposes any such parameter the body has not
// moved out of when it completes or is disposed". §29.11 (v0.359): "Inside a
// callee, a plain `IEnumerable<T>` parameter is statically untracked ... so a
// violation there is enforced only by the runtime `InvalidOperationException`
// guard."
//
// Before the fix neither half existed for such an iterable. The definite-
// assignment pass tracked §29.11's consumption points only for a value typed
// at the lazy-LINQ sentinel, and a binding of an iterator call is typed
// `IEnumerable<T>`. And the iterator's GetEnumerator armed the claim (move the
// captures to the clone, raise on a repeat) only in the #2154 / #973a shapes,
// so every other GetEnumerator memcpy'd the same parameter pointer: when the
// body moved the parameter into a `using var` local (the issue's
// `using var r := move b;`), the first enumerator disposed it and the second
// read the freed object. MEASURED on the base (1128406b7), bezelc
// --sanitize=address, loop x200: the literal repro and the `Twice` GenO row are
// `heap-use-after-free ... in bz$iter$M.Program$GenO$MoveNext`; the GenE rows
// compile and replay the enumeration.
//
// THE FIX. Static (DefiniteAssignment.cpp): a local initialized by a direct
// call to such an iterator shares the lazy queries' consumption tracking, and
// GetEnumerator() counts as a consumption point. Run time
// (emitIteratorMethod, LlvmBackend_Iterators.cpp): every such iterator at an
// iterator-format enumerable head arms the claim.
//
// THE ORACLE. Rejections assert the specific diagnostic (the consumed binding,
// the iterator, and the consuming use). Run-time rows are `runAotChecked`
// (generated-code ASan + LSan) and print a sum on stdout, the
// InvalidOperationException arm adding 1000 per iteration.

#include <initializer_list>
#include <string>

#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// True iff the program is REJECTED at compile time and one diagnostic contains
// every needle.
bool rejectedWith(const std::string& src,
                  std::initializer_list<const char*> needles) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        bool all = true;
        for (const char* n : needles)
            if (d.message.find(n) == std::string::npos) all = false;
        if (all) return true;
    }
    return false;
}

void expectRun(const std::string& program, const std::string& expected) {
    bezel::test::JitResult r = runAotChecked(program);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, expected);
}

}  // namespace

// The issue's literal repro: a bound `using var src := GenO(...)` passed twice
// as an `IEnumerable<int>` argument. The flow analysis sees the second pass, so
// §29.8 makes it a compile error. Base: compiles; heap-use-after-free in
// bz$iter$M.Program$GenO$MoveNext.
BEZEL_TEST(iterator_owned_param_single_consumption,
           literal_second_argument_pass_rejected_spec_29_8_8787) {
    const std::string program =
    "module M;\n"
    "public class Box\n"
    "  public V: int;\n"
    "  public constructor(v: int) V := v; end constructor;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method GenO(owned b: Box, f: (int) -> int): IEnumerable<int>\n"
    "    using var r := move b;\n"
    "    for var k in 0..<3 do yield return f(k) + r.V; end;\n"
    "  end GenO;\n"
    "  public static method Use(src: IEnumerable<int>): int\n"
    "    var t := 0;\n"
    "    for var x in src do t := t + x; end;\n"
    "    return t;\n"
    "  end Use;\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var a := i;\n"
    "      using var src := GenO(new Box(1), lambda (v: int) => v + a);\n"
    "      t := t + Use(src);\n"
    "      try\n"
    "        t := t + Use(src);\n"
    "      catch\n"
    "        InvalidOperationException => do t := t + 1000; end,\n"
    "      end try;\n"
    "    end;\n"
    "    Console.WriteLine($\"{t}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";
    BEZEL_EXPECT_TRUE(rejectedWith(
        program, {"iterable `src` was already consumed", "`GenO`",
                  "IEnumerable<T> argument"}));
}

// Consumption point `for`, twice, over an iterator whose `owned` parameter the
// frame disposes (no move in the body). Base: compiles and replays.
BEZEL_TEST(iterator_owned_param_single_consumption,
           second_for_rejected_spec_29_8_8787) {
    const std::string program =
    "module M;\n"
    "public class Box\n"
    "  public V: int;\n"
    "  public constructor(v: int) V := v; end constructor;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method GenO(owned b: Box, f: (int) -> int): IEnumerable<int>\n"
    "    using var r := move b;\n"
    "    for var k in 0..<3 do yield return f(k) + r.V; end;\n"
    "  end GenO;\n"
    "  public static method GenE(owned b: Box): IEnumerable<int>\n"
    "    for var k in 0..<3 do yield return k + b.V; end;\n"
    "  end GenE;\n"
    "  public static method Use(src: IEnumerable<int>): int\n"
    "    var t := 0;\n"
    "    for var x in src do t := t + x; end;\n"
    "    return t;\n"
    "  end Use;\n"
    "  public static method Twice(src: IEnumerable<int>): int\n"
    "    var t := Use(src);\n"
    "    try\n"
    "      t := t + Use(src);\n"
    "    catch\n"
    "      InvalidOperationException => do t := t + 1000; end,\n"
    "    end try;\n"
    "    return t;\n"
    "  end Twice;\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var src := GenE(new Box(1));\n"
    "      for var x in src do t := t + x; end;\n"
    "      for var x in src do t := t + x; end;\n"
    "    end;\n"
    "    Console.WriteLine($\"{t}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";
    BEZEL_EXPECT_TRUE(rejectedWith(
        program, {"iterable `src` was already consumed", "`GenE`",
                  "for-in enumeration"}));
}

// Consumption point `GetEnumerator()`, then a `for`: §29.11 point 1's
// "`var e := q.GetEnumerator();` spends `q`, and a `for var x in q` after it
// is the static error", which §29.8 applies to this iterable. Base: compiles.
BEZEL_TEST(iterator_owned_param_single_consumption,
           for_after_get_enumerator_rejected_spec_29_8_8787) {
    const std::string program =
    "module M;\n"
    "public class Box\n"
    "  public V: int;\n"
    "  public constructor(v: int) V := v; end constructor;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method GenO(owned b: Box, f: (int) -> int): IEnumerable<int>\n"
    "    using var r := move b;\n"
    "    for var k in 0..<3 do yield return f(k) + r.V; end;\n"
    "  end GenO;\n"
    "  public static method GenE(owned b: Box): IEnumerable<int>\n"
    "    for var k in 0..<3 do yield return k + b.V; end;\n"
    "  end GenE;\n"
    "  public static method Use(src: IEnumerable<int>): int\n"
    "    var t := 0;\n"
    "    for var x in src do t := t + x; end;\n"
    "    return t;\n"
    "  end Use;\n"
    "  public static method Twice(src: IEnumerable<int>): int\n"
    "    var t := Use(src);\n"
    "    try\n"
    "      t := t + Use(src);\n"
    "    catch\n"
    "      InvalidOperationException => do t := t + 1000; end,\n"
    "    end try;\n"
    "    return t;\n"
    "  end Twice;\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var src := GenE(new Box(1));\n"
    "      using var e := src.GetEnumerator();\n"
    "      while e.MoveNext() do t := t + e.Current; end;\n"
    "      for var x in src do t := t + x; end;\n"
    "    end;\n"
    "    Console.WriteLine($\"{t}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n";
    BEZEL_EXPECT_TRUE(rejectedWith(
        program, {"iterable `src` was already consumed", "`GenE`",
                  "for-in enumeration"}));
}

// Run time: one pass at the caller, two enumerations inside the untracked
// callee `Twice`. The second GetEnumerator raises InvalidOperationException,
// and nothing is read after it is freed. 200 x ((3i + 6) + 1000) = 260900.
// Base: heap-use-after-free in bz$iter$M.Program$GenO$MoveNext.
BEZEL_TEST(iterator_owned_param_single_consumption,
           callee_second_enumeration_raises_moved_param_spec_29_8_8787) {
    expectRun(
    "module M;\n"
    "public class Box\n"
    "  public V: int;\n"
    "  public constructor(v: int) V := v; end constructor;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method GenO(owned b: Box, f: (int) -> int): IEnumerable<int>\n"
    "    using var r := move b;\n"
    "    for var k in 0..<3 do yield return f(k) + r.V; end;\n"
    "  end GenO;\n"
    "  public static method GenE(owned b: Box): IEnumerable<int>\n"
    "    for var k in 0..<3 do yield return k + b.V; end;\n"
    "  end GenE;\n"
    "  public static method Use(src: IEnumerable<int>): int\n"
    "    var t := 0;\n"
    "    for var x in src do t := t + x; end;\n"
    "    return t;\n"
    "  end Use;\n"
    "  public static method Twice(src: IEnumerable<int>): int\n"
    "    var t := Use(src);\n"
    "    try\n"
    "      t := t + Use(src);\n"
    "    catch\n"
    "      InvalidOperationException => do t := t + 1000; end,\n"
    "    end try;\n"
    "    return t;\n"
    "  end Twice;\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0..<200 do\n"
    "      var a := i;\n"
    "      using var src := GenO(new Box(1), lambda (v: int) => v + a);\n"
    "      t := t + Twice(src);\n"
    "    end;\n"
    "    Console.WriteLine($\"{t}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n",
        "260900\n");
}

// The frame-disposed twin: the same callee over GenE (no move in the body).
// 200 x (6 + 1000) = 201200. Base: the second enumeration replays, 2400.
BEZEL_TEST(iterator_owned_param_single_consumption,
           callee_second_enumeration_raises_frame_disposed_param_spec_29_8_8787) {
    expectRun(
    "module M;\n"
    "public class Box\n"
    "  public V: int;\n"
    "  public constructor(v: int) V := v; end constructor;\n"
    "end Box;\n"
    "public class Program\n"
    "  public static method GenO(owned b: Box, f: (int) -> int): IEnumerable<int>\n"
    "    using var r := move b;\n"
    "    for var k in 0..<3 do yield return f(k) + r.V; end;\n"
    "  end GenO;\n"
    "  public static method GenE(owned b: Box): IEnumerable<int>\n"
    "    for var k in 0..<3 do yield return k + b.V; end;\n"
    "  end GenE;\n"
    "  public static method Use(src: IEnumerable<int>): int\n"
    "    var t := 0;\n"
    "    for var x in src do t := t + x; end;\n"
    "    return t;\n"
    "  end Use;\n"
    "  public static method Twice(src: IEnumerable<int>): int\n"
    "    var t := Use(src);\n"
    "    try\n"
    "      t := t + Use(src);\n"
    "    catch\n"
    "      InvalidOperationException => do t := t + 1000; end,\n"
    "    end try;\n"
    "    return t;\n"
    "  end Twice;\n"
    "  public static method Main(): int\n"
    "    var t := 0;\n"
    "    for var i in 0..<200 do\n"
    "      using var src := GenE(new Box(1));\n"
    "      t := t + Twice(src);\n"
    "    end;\n"
    "    Console.WriteLine($\"{t}\");\n"
    "    return 0;\n"
    "  end Main;\n"
    "end Program;\n",
        "201200\n");
}
