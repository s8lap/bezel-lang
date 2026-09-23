// Regression tests for issue #4747 — `FrozenArray<T>.ToArray()` was never
// typed by sema, so an UNANNOTATED local bound to it degraded to a
// zero-initialized slot and every later read of that local emitted NOTHING
// AT ALL (the #2434 / #3079 silent-degrade class).
//
// stdlib §4.19 (v0.307) declares on `FrozenArray<T>`:
//
//     public method ToArray(): owned Array<T>;   // fresh mutable copy back out
//
// MEASURED on the pre-fix tree with the real `build/bezelc`:
//
//     var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);
//     using var fa := FrozenArray<int>.From(move buf);
//     using var back := fa.ToArray();
//     back[0] := 99;
//     Console.WriteLine(back[0]);   // §4.19 requires 99 (a FRESH mutable copy)
//     Console.WriteLine(fa[0]);     // §4.19 requires 0  (the source is frozen)
//
//   ...compiled with exit 0, printed the single line "0", and emitted
//
//     warning: internal: the argument of this `Console.WriteLine` failed to
//     lower to a value, so the whole statement emits nothing — not even the
//     line terminator (stdlib §2.1 / §21.9) — #3079
//
// It is a TYPING gap, not a lowering gap: with an explicit `: Array<int>`
// annotation on the local the IDENTICAL call already lowered and ran
// correctly (the annotated-control test below pins that, and it passes on
// BOTH sides of the fix — it is a control, not the repro).
//
// Root cause: the `colName` lambda in ExprTypecheck_Calls.cpp's collection
// dispatch reads ONLY the declaration-LESS Named TypeRef shape
// (`!recvTy->declaration && recvTy->astNode`). `FrozenArray<T>.From(...)`'s
// result carries the builtinClassSym shape instead (`declaration =
// builtinClassSym("FrozenArray")`, no astNode — stamped by the #3249 block
// in the same file), so `colName` came back EMPTY for that receiver and no
// arm in the whole block ever typed `fa.ToArray()`.
//
// Cite: stdlib §4.19, §2.1; language spec §21.9, v0.307.

#include "support/aot_support.hpp"  // runAot, runAotOut
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// The issue's literal repro, verbatim from #4747's "Repro" section.
const char* kReproFrozenToArray = R"BZ(
module Conf;
public class Program
  public static method Main()
    var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);
    using var fa := FrozenArray<int>.From(move buf);
    using var back := fa.ToArray();
    back[0] := 99;
    Console.WriteLine(back[0]);
    Console.WriteLine(fa[0]);
  end Main;
end Program;
)BZ";

}  // namespace

// ═══════════ Primary bug — the issue's measured repro, asserted on STDOUT ══
// A compile-only assertion cannot tell "works" from "emits nothing": the
// pre-fix program compiled with exit 0. The oracle must therefore RUN and
// read the captured stdout.
//
// Pre-fix stdout: "0\n"        (one line; the 99 line vanished entirely)
// Post-fix stdout: "99\n0\n"   (stdlib §4.19: a FRESH mutable copy, and the
//                               frozen source untouched by the write)
BEZEL_TEST(frozen_array_toarray_typing_4747,
           unannotated_toarray_local_prints_both_lines_stdlib_4_19) {
    auto r = runAot(kReproFrozenToArray);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("99\n0\n"));
}

// ⚠ The "no `internal:` residual" oracle is deliberately NOT here. `runAot`
// populates `JitResult::diagnostics` only on a compile REJECT (or a compiler
// crash/timeout) — the compiler's stderr on a SUCCESSFUL compile goes to the
// harness's `cc.err` file and is discarded (aot_support.hpp, the
// `cachedCompile` result path). The pre-fix program COMPILED, so an assertion
// about its `#3079` warning in this file would be structurally incapable of
// failing. The silence oracle is
// tests/lit/frozen_array_toarray_typing_4747.bzl, which pipes the real
// `bezelc` stderr through FileCheck. (Same split, same reason, as #3098's.)

// The reduced form named in the issue: `Console.WriteLine(back.Count)` is
// enough to show it. Pre-fix this printed NOTHING (empty stdout, exit 0).
BEZEL_TEST(frozen_array_toarray_typing_4747,
           unannotated_toarray_count_read_prints_two_stdlib_4_19) {
    const std::string p = R"BZ(
module Conf;
public class Program
  public static method Main()
    var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);
    using var fa := FrozenArray<int>.From(move buf);
    using var back := fa.ToArray();
    Console.WriteLine(back.Count);
  end Main;
end Program;
)BZ";
    BEZEL_EXPECT_EQ(runAotOut(p), std::string("2"));
}

// §4.19's "fresh mutable copy" contract, exercised past the first element:
// mutating the copy must not be observable through the frozen source, and
// the copy must carry the source's contents (not a zero-filled slot).
BEZEL_TEST(frozen_array_toarray_typing_4747,
           unannotated_toarray_copy_is_independent_of_source_stdlib_4_19) {
    const std::string p = R"BZ(
module Conf;
public class Program
  public static method Main()
    var buf := new OwningBuffer<int>(3, lambda(i: int): int => i + 10);
    using var fa := FrozenArray<int>.From(move buf);
    using var back := fa.ToArray();
    back[1] := 77;
    Console.WriteLine(back[0]);
    Console.WriteLine(back[1]);
    Console.WriteLine(back[2]);
    Console.WriteLine(fa[1]);
  end Main;
end Program;
)BZ";
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("10\n77\n12\n11\n"));
}

// ═══════════ Control — the ANNOTATED form was already correct ═════════════
// This row passes on BOTH sides of the fix. It is here to pin that the fix
// did not disturb the already-working shape (and it is what proved the
// defect was a typing gap rather than a lowering gap).
BEZEL_TEST(frozen_array_toarray_typing_4747,
           annotated_toarray_local_control_unchanged_stdlib_4_19) {
    const std::string p = R"BZ(
module Conf;
public class Program
  public static method Main()
    var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);
    using var fa := FrozenArray<int>.From(move buf);
    using var back: Array<int> := fa.ToArray();
    back[0] := 99;
    Console.WriteLine(back[0]);
    Console.WriteLine(fa[0]);
  end Main;
end Program;
)BZ";
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("99\n0\n"));
}

// ═══════════ Control — the §26.5 rule-1 arity rejection still fires ═══════
// #3342 (stdlib §4.4 / spec §26.5) rejects `ToArray` with arity > 0 on an
// `Array<T>` or `FrozenArray<T>` receiver: the instance member blocks the
// same-named §5 LINQ extension. The new typing arm is gated on
// `ce.args.empty()`, so that rejection must be untouched.
BEZEL_TEST(frozen_array_toarray_typing_4747,
           toarray_with_argument_still_rejected_spec_26_5) {
    const std::string p = R"BZ(
module Conf;
public class Program
  public static method Main()
    var buf := new OwningBuffer<int>(2, lambda(i: int): int => i);
    using var fa := FrozenArray<int>.From(move buf);
    using var back := fa.ToArray(lambda(x: int): bool => x > 0);
    Console.WriteLine(back.Count);
  end Main;
end Program;
)BZ";
    auto r = runAot(p);
    BEZEL_EXPECT_FALSE(r.ran);
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find("instance members block same-named extensions") !=
            std::string::npos)
            found = true;
    }
    BEZEL_EXPECT_TRUE(found);
}

// ═══════════ Control — `Array<T>.ToArray()` (§4.5) is unaffected ══════════
// The new arm keys on the builtinClassSym `FrozenArray` receiver shape only;
// the mutable Array<T> path goes through the pre-existing `colName` arms.
BEZEL_TEST(frozen_array_toarray_typing_4747,
           array_toarray_copy_control_stdlib_4_5) {
    const std::string p = R"BZ(
module Conf;
public class Program
  public static method Main()
    using var a := new Array<int>();
    a.Add(5);
    a.Add(6);
    using var b := a.ToArray();
    b[0] := 50;
    Console.WriteLine(b[0]);
    Console.WriteLine(a[0]);
  end Main;
end Program;
)BZ";
    auto r = runAot(p);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("50\n5\n"));
}
