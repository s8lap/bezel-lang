// Codegen — issue #8831: the #7558 shape in the body kinds #7558's candidate
// fix left out. A literal-initialised `Array<int>` local that is CAPTURED by
// a lambda, where the lambda is called and dropped inside the same body, was
// never freed when the body is a property getter, an iterator, an async method
// or a lambda body. The array header and its data block (160 B in 2
// allocations) leaked once per call.
//
// THE RULE. §28.9 (v0.359, "Runtime-managed types"): a borrowing `Array<T>` is
// trivial-dispose, but its backing storage is part of the instance's single
// allocation and is released with it (§28.11, "Borrowing collections").
// §27.5 (v0.359, "Closures"): a lambda captures a variable by reference into
// a heap closure; a closure that does not outlive the capturing scope (called
// and dropped locally) holds no claim on the variable past that scope's end.
//
// ROOT CAUSE. The scope-end reclaim of a borrowing-`Array<T>` literal local is
// registered by `lowerLocalDecl` only when
// `collectBorrowingArrayFreeEligible`'s escape walk proved the local never
// escapes its scope. That walk counted ANY read of the local inside a lambda
// body as an escape ("the env outlives the scope"), so an array captured by a
// lambda that is proven non-escaping was treated like one stored in a field.
// Fix #7558 lifted that override for a SAFE READ (a receiver of a member /
// `?.` access, a comparison operand) of a local whose every capturing lambda
// is non-escaping — but the lift was wired into the walk through the
// `lambdaCaptureBorrow` opt-in, and only `emitFunctionBody` and
// `emitConstructorBody` passed it. Every other body kind kept the old rule,
// so the same shape leaked there (issue #8831). Two of those venues had a
// second gap: the iterator and the async-iterator ran
// `collectBorrowingArrayFreeEligible` BEFORE `collectCellFreeEligible`, so
// the predicate's oracle (`cellFreeEligible_`) did not yet describe that body
// and could never have held.
//
// THE FIX. The predicate is enabled at the missing venues, and the two
// iterator venues' call is moved after `collectCellFreeEligible` — the order
// every other venue already uses. The getter bodies (the standalone getter,
// the vtable/itable slot face and the indexer face) get the pass itself: they
// never ran it, so a plain literal in a getter body was never reclaimed at
// all. The fix-forward guards of #7558 are inherited unchanged: the lift
// covers only a read whose chain reaches a VALUE COPY (a `string` step is not
// a copy, and a receiver read whose result is itself a borrow of the array —
// `a.GetEnumerator()`, `a.AsReadOnlySpan()` — is not one), and an async
// lambda's body is never lifted.
//
// ORACLE. `runAotChecked` (generated-code AddressSanitizer + LeakSanitizer):
// a leak of this shape is reported as a failed run, so a RED row here is the
// leak and a GREEN row is its absence. The rows call the leaking shape 200
// times, so a leak is 400 allocations and cannot hide under the sanitizer's
// noise (one leak of this shape is reported only intermittently).
//
// The two BOUNDARY CONTROLS are base-green by construction — the sound
// codegen verdict for an array that ESCAPES is the conservative one, which
// keeps (leaks) it. Each is red under a plausible wrong fix named in the
// commit body.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// stdout of a checked run (ASan + LSan) that compiled, ran and exited 0, or a
// marker naming what failed.
std::string checkedOut(const std::string& program) {
    auto r = runAotChecked(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

// stdout of a lenient run (ASan, no LSan) that compiled, ran and exited 0, or
// a marker naming what failed.
std::string lenientOut(const std::string& program) {
    auto r = runAotCheckedLenient(program);
    if (!r.ran) return "<did-not-run>";
    if (r.exitCode != 0) return "<exit " + std::to_string(r.exitCode) + ">";
    return r.stdoutText;
}

}  // namespace

// The issue's first repro: a block property GETTER whose body declares the
// literal and captures it with a lambda. 200 reads * 3 elements = 600.
//
// RED on the base (r30-c1): "AddressSanitizer: 32000 byte(s) leaked in 400
// allocation(s)".
BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           issue_repro_getter_body_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class C
  public property N: int
    get => do
      var a: Array<int> := [1,2,3];
      var f := lambda() => a.Count;
      return f();
    end;
  end;
end C;
class P
  public static method Main(): int
    var s := 0;
    var c := new C();
    for var i in 1..200 do s := s + c.N; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// The issue's second repro: the same three lines in an ITERATOR body.
// 200 enumerations * 3 = 600.
BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           issue_repro_iterator_body_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method Gen(): IEnumerable<int>
    var a: Array<int> := [1,2,3];
    var f := lambda() => a.Count;
    yield return f();
  end Gen;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do for var x in Gen() do s := s + x; end; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// The issue's third repro: the same three lines in a `static async method`.
// 200 calls * 3 = 600.
BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           issue_repro_async_method_body_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
import System.Threading.Tasks;
class P
  static async method One(): Task<int>
    var a: Array<int> := [1,2,3];
    var f := lambda() => a.Count;
    return f();
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One().Result; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// The issue's fourth repro: the array declared inside an OUTER lambda's block
// body and captured by a NESTED lambda. 200 calls * 3 = 600.
BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           issue_repro_lambda_body_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class P
  static method One(): int
    var outer := lambda(): int => do
      var a: Array<int> := [1,2,3];
      var f := lambda() => a.Count;
      return f();
    end;
    return outer();
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// The ASYNC-LAMBDA sibling of the row above: the same three lines in an
// `async lambda` body. 200 calls * 3 = 600.
BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           async_lambda_body_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
import System.Threading.Tasks;
class P
  static method One(): int
    var g := async lambda(): Task<int> => do
      var a: Array<int> := [1,2,3];
      var f := lambda() => a.Count;
      return f();
    end;
    return g().Result;
  end One;
  public static method Main(): int
    var s := 0;
    for var i in 1..200 do s := s + One(); end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// The VTABLE/ITABLE SLOT FACE of the block getter — the same body, emitted a
// second time as the dispatch slot of a `virtual` property. 200 reads * 3 = 600.
BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           virtual_getter_face_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class C
  public virtual property N: int
    get => do
      var a: Array<int> := [1,2,3];
      var f := lambda() => a.Count;
      return f();
    end;
  end;
end C;
class P
  public static method Main(): int
    var s := 0;
    var c := new C();
    for var i in 1..200 do s := s + c.N; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// The INDEXER face of the block getter. 200 reads * 3 = 600.
BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           indexer_getter_face_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
class H
  public n: int;
  public property [i: int]: int
    get => do
      var a: Array<int> := [1,2,3];
      var f := lambda() => a.Count;
      return n + f();
    end;
  end;
end H;
class P
  public static method Main(): int
    var s := 0;
    var h := new H();
    for var i in 1..200 do s := s + h[0]; end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "600\n");
}

// ---------------------------------------------------------------------------
// BOUNDARY CONTROL 1 of 2: a read whose result carries the array's storage
// OUT of the body keeps the array alive.
//
// THE RULE. §28.9 (v0.359): runtime management of a closure "relaxes nothing
// about the values the closure captures: a captured borrowed class reference
// is subject to §28.3's borrow-escape rules". `a.AsReadOnlySpan()` (the
// getter spelling) and `a.GetEnumerator()` (the async-method spelling) are
// BORROWS of the array's backing storage, not copies of it — so the array
// must NOT be reclaimed at the body's scope end while the view the lambda
// returned is still read by the caller.
//
// The control pins MEMORY SAFETY and the output under
// `runAotCheckedLenient` (ASan on, LSan off): the sound codegen verdict for
// an array that ESCAPES is the conservative one, which keeps (leaks) it. It is
// red under a plausible wrong fix: lifting the capture escape verdict for
// EVERY read inside a lambda body, whatever the read's result type (the view
// then reads reclaimed storage — heap-use-after-free).
//
// It is ONE row with TWO spellings (the getter's span and the async
// method's enumerator), so the suite's base-green budget covers both:
// 100 * ((1 + 3) + (1 + 2 + 3)) = 1000.
// ---------------------------------------------------------------------------

BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           boundary_control_escape_views_keep_the_array_alive_spec_28_9) {
    BEZEL_EXPECT_EQ(lenientOut(R"BZL(
module M; import System; import System.Linq; import System.Collections.Generic;
import System.Threading.Tasks;
class C
  public property Sp: ReadOnlyArraySpan<int>
    get => do
      var a: Array<int> := [1,2,3];
      var f := lambda() => a.AsReadOnlySpan();
      return f();
    end;
  end;
end C;
class P
  static async method One(): Task<IEnumerator<int>>
    var a: Array<int> := [1,2,3];
    var f := lambda() => a.GetEnumerator();
    return f();
  end One;
  public static method Main(): int
    var s := 0;
    var c := new C();
    for var i in 1..100 do
      var sp := c.Sp; s := s + sp[0] + sp[2];
      var e := One().Result; while e.MoveNext() do s := s + e.Current; end;
    end;
    Console.WriteLine(s); return 0; end Main; end P;
)BZL"),
              "1000\n");
}

// ---------------------------------------------------------------------------
// BOUNDARY CONTROL 2 of 2: the ENCLOSING body's own literal is still
// reclaimed after a getter body has run.
//
// THE RULE. §28.9 (v0.359): the backing storage of a borrowing `Array<T>` is
// reclaimed with the instance at the OWNER's scope end. The owner here is the
// METHOD, not the getter: a getter is emitted (lazily) in the MIDDLE of
// another body, so the method's own eligibility verdict must survive the
// nested emission unchanged.
//
// The control is a LEAK check (`runAotChecked`, ASan + LSan): the method's
// literal is declared AFTER the property read, so its scope-end registration
// is made after the getter has been emitted. The getter's own body declares
// NO collection literal, so the only array in the program is the method's —
// the row is therefore green on the base arm (the method venue has always
// reclaimed it) and green with the fix. It is red under a plausible wrong
// fix: omitting the save/restore of the borrowing-reclaim sets around the
// nested body emission (the getter's own verdict and fn token would still be
// in place, so the method's registration would never fire and the method's
// literal would leak).
//
// 200 calls * 7 = 1400.
// ---------------------------------------------------------------------------

BEZEL_TEST(captured_array_literal_lambda_leak_8831,
           boundary_control_enclosing_body_literal_still_reclaimed_spec_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(R"BZL(
module M;
class C
  public property N: int
    get => do
      var k := 7;
      return k;
    end;
  end;
end C;
class P
  static method One(): int
    var c := new C();
    var s := c.N;
    var ys: Array<int> := [1, 2];
    return s + ys.Count - 2;
  end One;
  public static method Main(): int
    var t := 0;
    for var i in 1..200 do t := t + One(); end;
    Console.WriteLine(t); return 0; end Main; end P;
)BZL"),
              "1400\n");
}
