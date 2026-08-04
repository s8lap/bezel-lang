// Codegen — issue #2298 (spec §28.9, v0.236): `wrapAsIEnumerableForTarget`
// allocated a vtable-backed enumerator STRUCT (`{ vtable, backing, idx, esz }`,
// 32 bytes on x86-64) for every collection passed as an `IEnumerable<T>`
// ARGUMENT, but registered a scope-end disposal for it ONLY when the wrap was
// owning (lazy pipeline drain, dict-backed Set snapshot). The borrowing wraps
// (Array, Stack, Queue, LinkedList, class-element Set) silently leaked their
// wrapper frame — a flat 32 bytes per call, unbounded inside loops. The local-
// decl form `var seq: IEnumerable<int> := someArray;` already handled the same
// wrap via `registerIterableSourceDisposal` (#943/#1911) — only the argument
// position was uncovered.
//
// Verified on main (HEAD `18efbc8`) with `scripts/agent_check.sh --probe
// --asan /tmp/probe.bzl`:
//
//   public static method Sum(xs: IEnumerable<int>): int
//     var n: int := 0;
//     for var x in xs do n := n + x; end;
//     return n;
//   end Sum;
//   public static method Main(): int
//     using var s := new Array<int>();
//     s.Add(5); s.Add(6);
//     return C.Sum(s);
//   end Main;
//
// Direct leak of 32 byte(s) in 1 object(s) allocated from:
//     #0 calloc
//     #1 main
//
//   32 byte(s) leaked in 1 allocation(s).
//
// Re-tested identically with `LinkedList<int>` (wrapLinkedListAsIEnumerable,
// same `free(self)` slot-1 Dispose, same gap).
//
// Fix (src/codegen/LlvmBackend_Assign.cpp, wrapAsIEnumerableForTarget's
// `outWrapMade` report + buildOrderedArgs' iterEnumDispose registration): the
// wrap function now reports "wrap made" for EVERY branch (owning AND
// borrowing), and buildOrderedArgs registers the same scope-end iterEnumDispose
// for the wrapper frame in both shapes. The non-owning slot-1 Dispose
// (`free(self)` — only the frame, never the shared backing) is exactly-once:
// the source collection's owner (a `using var`, the local variable, etc.)
// retains the disposal obligation for the backing, and the wrapper frame is
// freed exactly once at the caller scope's end.
//
// Fail-without / pass-with: every test below reports 32B (Array/Stack/Queue) or
// 32B (LinkedList) leak under LSan on the pre-fix compiler (the -999 sentinel
// from runAotChecked's ASan net); after the fix each returns the asserted VALUE
// with NO leak reported (exit 0).
// // per spec §28.9 (issue #2298)

#include <string>

#include "support/codegen_support.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// runAotChecked runs the linked executable under generated-code ASan; the pre-
// fix leak flips r.ran to false -> -999, distinct from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

const char* kSum =
    "module M; class C "
    "  public static method Sum(xs: IEnumerable<int>): int "
    "    var n: int := 0; "
    "    for var x in xs do n := n + x; end; "
    "    return n; "
    "  end Sum; ";

// ===========================================================================
// spec §28.9 / fix #2298 — borrowing wrap frame leaks
// ===========================================================================

// Array<T> argument: the wrapAsIEnumerableForTarget borrowing wrap (32B struct)
// leaks per call. Before the fix: 32B leak at `main` (calloc). After the
// fix: leak-free. The SUM itself is unaffected — only the wrapper frame is
// freed by the iterEnumDispose registered at argument scope end. 5 + 6 = 11.
BEZEL_TEST(wrap_argument_frame_leak_2298,
           array_as_ienumerable_argument_no_wrapper_leak_spec_28_9) {
    std::string src = std::string(kSum) +
        "public static method Main(): int "
        "  using var s := new Array<int>(); "
        "  s.Add(5); s.Add(6); "
        "  return C.Sum(s); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// Stack<T> argument: wrapStackAsIEnumerator (reuses #2471's reverse machinery,
// slot-1 Dispose is also `free(self)` — only the frame). Same gap. Before the
// fix: 32B leak. After the fix: leak-free.
BEZEL_TEST(wrap_argument_frame_leak_2298,
           stack_as_ienumerable_argument_no_wrapper_leak_spec_28_9) {
    std::string src =
        "module M; import System.Collections; class C "
        "  public static method Sum(xs: IEnumerable<int>): int "
        "    var n: int := 0; "
        "    for var x in xs do n := n + x; end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var s := new Stack<int>(); "
        "    s.Push(5); s.Push(6); "
        "    return C.Sum(s); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// Queue<T> argument: same path as Array (wrapArrayAsIEnumerable; Queue is in
// the array-backed family). Same gap. Before the fix: 32B leak. After the
// fix: leak-free.
BEZEL_TEST(wrap_argument_frame_leak_2298,
           queue_as_ienumerable_argument_no_wrapper_leak_spec_28_9) {
    std::string src =
        "module M; import System.Collections; class C "
        "  public static method Sum(xs: IEnumerable<int>): int "
        "    var n: int := 0; "
        "    for var x in xs do n := n + x; end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var s := new Queue<int>(); "
        "    s.Add(5); s.Add(6); "
        "    return C.Sum(s); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 11);
}

// REGRESSION the fix must NOT disturb: the OWNING-wrap path (lazy LINQ
// pipeline). The OWNING wrap was already disposed exactly once before #2298
// (via outOwningWrap/owningArgWrap — see #1923); the rename to outWrapMade
// preserves the same scope-end iterEnumDispose registration. A `Select`
// argument to `Sum` must enumerate every element, dispose the drained array
// once at argument scope end, and return the asserted sum. 2 + 3 = 5.
BEZEL_TEST(wrap_argument_frame_leak_2298,
           lazy_pipeline_argument_still_disposed_once_spec_28_9) {
    std::string src =
        "module M; "
        "import System.Collections.Linq; "
        "class C "
        "  public static method Sum(xs: IEnumerable<int>): int "
        "    var n: int := 0; "
        "    for var x in xs do n := n + x; end; "
        "    return n; "
        "  end Sum; "
        "  public static method Main(): int "
        "    using var xs: Array<int> := [2, 3]; "
        "    return C.Sum(xs.Select(lambda(n) => n)); "
        "  end Main; "
        "end C;";
    BEZEL_EXPECT_EQ(run(src), 5);
}

// REGRESSION the fix must NOT disturb: a callee-owned Array<T> RETURN is a
// DIFFERENT site (covered by #2299). Pin the local-decl form which already
// disposed the wrapper correctly before #2298: a `var seq := someArray;`
// pattern, for-in over `seq`, scope-end reuse proves the wrap is reusable
// (the for-in clones via slot-4 GetEnumerator, never touches the source).
BEZEL_TEST(wrap_argument_frame_leak_2298,
           local_decl_borrowing_wrap_dispose_unchanged_spec_28_9) {
    std::string src =
        "module M; class C "
        "  public static method Main(): int "
        "    var n1: int := 0; "
        "    var n2: int := 0; "
        "    using var s := new Array<int>(); "
        "    s.Add(5); s.Add(6); "
        "    var seq: IEnumerable<int> := s; "
        "    for var x in seq do n1 := n1 + x; end; "
        "    for var x in seq do n2 := n2 + x; end; "
        "    return n1 + n2; "
        "  end Main; "
        "end C;";
    // 5 + 6 = 11, twice (re-iterable).
    BEZEL_EXPECT_EQ(run(src), 22);
}

}  // namespace
