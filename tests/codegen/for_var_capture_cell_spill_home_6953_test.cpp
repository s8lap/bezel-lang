// Codegen — issue #6953 follow-up (§27.5 Closures, §21.4 The `for`
// statement, §30.1 `async` methods, §30.7 Async iterators, spec v0.359): a
// lambda capturing the loop variable of a `for` over a COLLECTION, in an async
// method or async iterator whose body suspends after the capture.
//
// Mechanism: in such a body the for-in element slot has no state-machine frame
// field; its cross-suspension home is the handler SPILL AREA
// (asyncHandlerSpillSlots_, promoted into the calloc'd `$Resume.spill` struct).
// lowerForBody now pre-boxes a captured for-var in state-machine bodies too
// (#6953) and frees, or refcount-releases (#7611), the cell at the iteration's
// body end, AFTER the resume. The cell HOLDER it creates must therefore get
// the same home as the element slot, as lowerLambda does for its own lazy box
// (#3503); a bare entry alloca of the resume function does not survive the
// suspension, and the body-end free then reads whatever the fresh
// activation's stack holds (ASan bad-free / double-free / SEGV).
//
// The suite iterator_for_var_capture_cell_free_6953 covers range loops only,
// whose for-var HAS a frame field; this suite covers the collection loops.
// The non-CONTROL rows use runAotChecked (generated-code ASan +
// LeakSanitizer) and are red on the base, where the lazily boxed cell leaked
// one cell per iteration. The two CONTROL rows use runAotCheckedLenient (the
// lambda escapes, so its closure is a separate, pre-existing leak): green on
// the base, they go red under the wrong fix "pre-box in state-machine bodies
// without giving the holder its spill-area home" (measured: SEGV in the
// allocator's Deallocate, no output).
// per §27.5, §21.4, §30.1, §30.7 (issue #6953)

#include "support/aot_support.hpp"   // runAotChecked / runAotCheckedLenient
#include "test_harness.hpp"

using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
std::string lenientOut(const std::string& prog) {
    auto r = runAotCheckedLenient(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// Async iterator, `for` over an Array<int>: capture, `await`, then
// `yield return f()`, 200 elements. Base: 199000 and a 4-B cell leaked per
// element.
// per §27.5, §21.4, §30.7 (issue #6953)
BEZEL_TEST(for_var_capture_cell_spill_home_6953,
           async_iterator_for_in_array_capture_await_yield_x200_spec_27_5_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static async method Gen(xs: Array<int>): IAsyncEnumerable<int> "
        "    for var y in xs do "
        "      var f := lambda(): int => y * 10; "
        "      await Task.Delay(0); "
        "      yield return f(); "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := []; "
        "    for var i in 0..<200 do xs.Add(i); end; "
        "    var total := 0; "
        "    await for var v in Gen(xs) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "199000\n");
}

// Two suspensions per element (`await`, `yield`, `await`, `yield`) in a `for`
// over an Array<int>, then a range `for` in the same async iterator. Base:
// a wrong total and leaked cells.
// per §27.5, §21.4, §30.7 (issue #6953)
BEZEL_TEST(for_var_capture_cell_spill_home_6953,
           async_iterator_for_in_array_two_suspensions_then_range_loop_spec_27_5_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static async method Gen(xs: Array<int>): IAsyncEnumerable<int> "
        "    for var y in xs do "
        "      var f := lambda(): int => y * 10; "
        "      await Task.Delay(0); "
        "      yield return 0; "
        "      await Task.Delay(0); "
        "      yield return f(); "
        "    end; "
        "    for var z in 0..<3 do "
        "      var g := lambda(): int => z; "
        "      yield return g(); "
        "      await Task.Delay(0); "
        "      yield return g(); "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := []; "
        "    for var i in 0..<200 do xs.Add(i); end; "
        "    var total := 0; "
        "    await for var v in Gen(xs) do total := total + v; end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "199006\n");
}

// The same over an Array<string>, the async iterator drained 200 times; the
// lambda reads the element's Length after the resume.
// per §27.5, §21.4, §30.7 (issue #6953)
BEZEL_TEST(for_var_capture_cell_spill_home_6953,
           async_iterator_for_in_string_array_capture_await_yield_x200_spec_27_5_30_7) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static async method Gen(xs: Array<string>): IAsyncEnumerable<int> "
        "    for var s in xs do "
        "      var f := lambda(): int => s.Length; "
        "      await Task.Delay(0); "
        "      yield return f(); "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<string> := [\"a\", \"bb\", \"ccc\"]; "
        "    var total := 0; "
        "    for var r in 0..<200 do "
        "      await for var v in Gen(xs) do total := total + v; end; "
        "    end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "1200\n");
}

// Async METHOD, `for` over an Array<int>: capture, `await`, then call the
// lambda; looped 50 times so the resume really runs on a fresh activation.
// Base: a 4-B cell leaked per element.
// per §27.5, §21.4, §30.1 (issue #6953)
BEZEL_TEST(for_var_capture_cell_spill_home_6953,
           async_method_for_in_array_capture_await_call_x50_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; "
        "class C "
        "  public static async method Sum(xs: Array<int>): Task<int> "
        "    var t := 0; "
        "    for var y in xs do "
        "      var f := lambda(): int => y * 10; "
        "      await Task.Delay(0); "
        "      t := t + f(); "
        "    end; "
        "    return t; "
        "  end Sum; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := []; "
        "    for var i in 0..<200 do xs.Add(i); end; "
        "    var r := 0; "
        "    for var k in 0..<50 do r := r + await Sum(xs); end; "
        "    Console.WriteLine(r); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "9950000\n");
}

// CONTROL: an async iterator over an Array<int> yields the lambda itself
// (escaping), and the consumer calls every one after the iterator finished.
// Red (SEGV, no output) under the wrong fix named in the header.
// per §27.5, §21.4, §30.7 (issue #6953)
BEZEL_TEST(for_var_capture_cell_spill_home_6953,
           CONTROL_async_iterator_for_in_array_yields_escaping_lambda_spec_27_5_30_7) {
    BEZEL_EXPECT_EQ(lenientOut(
        "module M; "
        "class C "
        "  public static async method Gen(xs: Array<int>): IAsyncEnumerable<() -> int> "
        "    for var y in xs do "
        "      var f := lambda(): int => y * 10; "
        "      await Task.Delay(0); "
        "      yield return f; "
        "    end; "
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := []; "
        "    for var i in 0..<200 do xs.Add(i); end; "
        "    var keep: Array<() -> int> := []; "
        "    await for var g in Gen(xs) do keep.Add(g); end; "
        "    var total := 0; "
        "    for var g in keep do total := total + g(); end; "
        "    Console.WriteLine(total); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "199000\n");
}

// CONTROL: an async method over an Array<int> stores the lambda (escaping)
// before an `await` and calls it after the resume and again after the method
// returned. Red (SEGV, no output) under the wrong fix named in the header.
// per §27.5, §21.4, §30.1 (issue #6953)
BEZEL_TEST(for_var_capture_cell_spill_home_6953,
           CONTROL_async_method_for_in_array_stores_lambda_across_await_spec_27_5_30_1) {
    BEZEL_EXPECT_EQ(lenientOut(
        "module M; "
        "class C "
        "  public static async method Sum(xs: Array<int>, keep: Array<() -> int>): Task<int> "
        "    var t := 0; "
        "    for var y in xs do "
        "      var f := lambda(): int => y * 10; "
        "      keep.Add(f); "
        "      await Task.Delay(0); "
        "      t := t + f(); "
        "    end; "
        "    return t; "
        "  end Sum; "
        "  public static async method Main(): Task<int> "
        "    var xs: Array<int> := []; "
        "    for var i in 0..<200 do xs.Add(i); end; "
        "    var keep: Array<() -> int> := []; "
        "    var r := await Sum(xs, keep); "
        "    var s := 0; "
        "    for var g in keep do s := s + g(); end; "
        "    Console.WriteLine(r); "
        "    Console.WriteLine(s); "
        "    return 0; "
        "  end Main; "
        "end C; "),
        "199000\n199000\n");
}
