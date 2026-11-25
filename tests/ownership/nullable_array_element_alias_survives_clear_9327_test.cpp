// Regression test for issue #9327 — a NULLABLE function-typed local bound from
// an `Array` element read took no reference of its own, so an operation that
// releases the array's reference to the element (`Clear()`, `RemoveAt(i)`)
// left the local naming a freed closure: `var g := arr[0]; arr.Clear(); g!()`
// was a heap-use-after-free. The non-nullable spelling `Array<() -> int>` was
// clean.
//
// THE RULE. §28.9 (v0.359): a function value is runtime-managed, and its
// lifetime ends when the LAST holder lets go. §28.11 / stdlib `Array.Clear`:
// the array releases its own reference to each element. A local read out of the
// array before that call is a second holder and must keep the value alive
// across it.
//
// THE DEFECT. `collectClosureReleaseEligible` (src/codegen/LlvmBackend_Setup.cpp)
// admits a function-typed local bound from a borrowed read as an OWNED ALIAS
// (it takes its own reference at the bind, `closureAliasOwned_`, fix #8440 /
// #8479) only for a NON-nullable declared type. A nullable one stayed a plain
// borrow. Its overwrite (`g := arr[1];`) was also a plain store
// (lowerClosureLocalReassign's nullable bail), so an ASSIGNED spelling
// (`var g: (() -> int)? := null; g := arr[0];`) took no reference either.
//
// THE FIX. A nullable function-typed local initialised from an `Array` element
// read, or initialised `null` and assigned (`:=`) from one, is an owned alias.
// Its `:=` overwrites retain the new value and release the old one
// (`ownedNullableAliasTarget`). The decision reads the declaration's own
// AST-level type, never a type parameter resolved at an instantiation, so a
// generic body shared by several instantiations decides the same way at each
// (row 8). A compound `??=` overwrite and a lambda-captured alias keep the
// pre-fix behaviour (a plain store / the capture machinery), and are not
// pinned here.
//
// ORACLE. `runAotChecked` (ASan over the generated module + LeakSanitizer);
// each loop runs 200x so a leak or a use-after-free cannot hide under sanitizer
// noise, and each row asserts the program's own output.
//
// per §28.9, §28.11, v0.359 (issue #9327)

#include <iostream>
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing and AddressSanitizer did not fire.
void assertNoSanitizerReport(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary =
        bezel::test::aot_detail::parseLeakSummary(text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// The program compiled, the sanitizer leg really ran (a toolchain SKIP fails
// the row), it exited normally with the exact expected output, and neither
// sanitizer fired.
void expectClean(const JitResult& r, const std::string& out) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, out);
    assertNoSanitizerReport(r);
}

}  // namespace

// ROW 1 — the ISSUE'S OWN REPRO: `var g := arr[0]; arr.Clear(); g!()`.
// 200 x 2 = 400.
//
// RED on the base: heap-use-after-free on the first `g!()`.
BEZEL_TEST(nullable_array_element_alias_survives_clear_9327,
           issue_repro_initialised_alias_survives_clear_spec_28_9) {
    const std::string src = R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
      arr.Add(lambda() => 2);
      var g := arr[0];
      arr.Clear();
      acc := acc + g!();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
    expectClean(runAotChecked(src), "400\n");
}

// ROW 2 — the ASSIGNED spelling the review `r43-rev-c67-9172-ff3` found
// (probe z06d): `var g: (() -> int)? := null; … g := a[0]; a.Clear(); g!()`.
// 200 x 2 = 400.
//
// RED on the base: heap-use-after-free.
BEZEL_TEST(nullable_array_element_alias_survives_clear_9327,
           assigned_alias_survives_clear_spec_28_9) {
    const std::string src = R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      using var a: Array<(() -> int)?> := new Array<(() -> int)?>();
      var g: (() -> int)? := null;
      a.Add(lambda() => 2);
      g := a[0];
      a.Clear();
      acc := acc + g!();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
    expectClean(runAotChecked(src), "400\n");
}

// ROW 3 — an alias REASSIGNED in a loop, then read after `Clear()`. Each
// overwrite takes the new element's reference and gives the old one back, and
// the last one is released at scope end. 200 x 2 = 400.
//
// RED on the base: heap-use-after-free.
BEZEL_TEST(nullable_array_element_alias_survives_clear_9327,
           alias_reassigned_in_a_loop_survives_clear_spec_28_9) {
    const std::string src = R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
      arr.Add(lambda() => 2);
      arr.Add(lambda() => 3);
      var g := arr[0];
      for var j in 0..<2 do
        g := arr[j];
      end for;
      g := arr[0];
      arr.Clear();
      acc := acc + g!();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
    expectClean(runAotChecked(src), "400\n");
}

// ROW 4 — two CAPTURING closures read into aliases, then `RemoveAt(0)` and
// `Clear()`: the closures' environments must survive both. Each iteration
// computes (k + 1) + k - 2k - 1 = 0, so the total is 0.
//
// RED on the base: heap-use-after-free.
BEZEL_TEST(nullable_array_element_alias_survives_clear_9327,
           capturing_closures_survive_removeat_and_clear_spec_28_9) {
    const std::string src = R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      var k := i + 1;
      using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
      arr.Add(lambda() => k);
      arr.Add(lambda() => k + 1);
      var g := arr[1];
      var h := arr[0];
      arr.RemoveAt(0);
      arr.Clear();
      acc := acc + g!() + h!() - 2 * k - 1;
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
    expectClean(runAotChecked(src), "0\n");
}

// ROW 5 — the alias RETURNED from the method that cleared the array: the
// caller's value must still be callable, and the reference the alias took at
// the bind must be accounted for on the return. 200 x 2 = 400.
//
// RED on the base: heap-use-after-free in the caller.
BEZEL_TEST(nullable_array_element_alias_survives_clear_9327,
           returned_alias_outlives_the_cleared_array_spec_28_9) {
    const std::string src = R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Get(arr: Array<(() -> int)?>): (() -> int)?
    var g := arr[0];
    arr.Clear();
    return g;
  end Get;
  public static method GetAssigned(arr: Array<(() -> int)?>): (() -> int)?
    var g: (() -> int)? := null;
    g := arr[0];
    arr.Clear();
    return g;
  end GetAssigned;
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
      arr.Add(lambda() => 2);
      var r := Program.Get(arr);
      acc := acc + r!();
      using var arr2: Array<(() -> int)?> := new Array<(() -> int)?>();
      arr2.Add(lambda() => 3);
      var r2 := Program.GetAssigned(arr2);
      acc := acc + r2!() - 3;
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
    expectClean(runAotChecked(src), "400\n");
}

// ROW 6 — the assigned alias takes EVERY kind of assignment: a borrowed element
// read, a `null` element, a fresh lambda (a producer, transferred), and another
// element read. The final value (arr[0] -> 2) is read after `Clear()`.
// 200 x 2 = 400.
//
// RED on the base: heap-use-after-free.
BEZEL_TEST(nullable_array_element_alias_survives_clear_9327,
           assigned_alias_mixed_assignments_survive_clear_spec_28_9) {
    const std::string src = R"BZL(
module P;
import System;
import System.Collections;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      using var arr: Array<(() -> int)?> := new Array<(() -> int)?>();
      arr.Add(lambda() => 2);
      arr.Add(null);
      var g: (() -> int)? := null;
      if i < 100 then
        g := arr[0];
      else
        g := lambda() => 5;
      end if;
      g := arr[1];
      g := arr[0];
      arr.Clear();
      acc := acc + g!();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
    expectClean(runAotChecked(src), "400\n");
}

// ROW 7 — the array is a FIELD of the receiver (`this.items[0]`) and the
// alias is a `const` as well as a `var`: the read is an `Array` element read
// whatever its receiver expression. 200 x (2 + 2) = 800.
//
// RED on the base: heap-use-after-free.
BEZEL_TEST(nullable_array_element_alias_survives_clear_9327,
           field_array_const_and_var_aliases_survive_clear_spec_28_9) {
    const std::string src = R"BZL(
module P;
import System;
import System.Collections;
public class Box
  public owned items: Array<(() -> int)?> := new Array<(() -> int)?>();
  public method Run(): int
    items.Add(lambda() => 2);
    const c := items[0];
    var g := this.items[0];
    items.Clear();
    return g!() + c!();
  end Run;
end Box;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      var b := new Box();
      acc := acc + b.Run();
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
    expectClean(runAotChecked(src), "800\n");
}

// ROW 8 (a base-green CONTROL, kept because it catches a plausible wrong fix
// the review of #9172 refuted four times) — a GENERIC body whose nullable
// function-typed alias is instantiated at several type arguments, in both
// orders. A fix that keyed the alias on the shared declaration and then read
// the instantiated type would retain / release a non-function `T` (an
// over-release or a stack-buffer-overflow). `FnAt0<T>` is a nullable
// function-typed alias at every instantiation; `At0N<T>` has a bare `T?`
// element, which is not a function type at any of them. 200 x
// (2 + 1 + 2 + 4 + 1 + 2) = 2400.
BEZEL_TEST(nullable_array_element_alias_survives_clear_9327,
           generic_alias_instantiated_at_function_int_and_string_spec_14_1) {
    const std::string src = R"BZL(
module P;
import System;
import System.Collections;
public class G
  public static method At0N<T>(a: Array<T?>): int
    var g := a[0];
    a.Clear();
    return 1;
  end At0N;
  public static method FnAt0<T>(a: Array<(() -> T)?>, d: T): T
    var g := a[0];
    if g is null then
      return d;
    end if;
    return g!();
  end FnAt0;
end G;
public class Program
  public static method Main(): int
    var acc := 0;
    for var i in 0..<200 do
      using var ia: Array<int?> := new Array<int?>();
      ia.Add(5);
      using var sa: Array<(() -> string)?> := new Array<(() -> string)?>();
      sa.Add(lambda() => "ab");
      using var fa: Array<(() -> int)?> := new Array<(() -> int)?>();
      fa.Add(lambda() => 2);
      acc := acc + G.At0N<int>(ia) + G.FnAt0<string>(sa, "").Length;
      acc := acc + G.FnAt0<int>(fa, 0);
      using var ia2: Array<(() -> int)?> := new Array<(() -> int)?>();
      ia2.Add(lambda() => 4);
      using var sb: Array<string?> := new Array<string?>();
      sb.Add("x");
      acc := acc + G.FnAt0<int>(ia2, 0) + G.At0N<string>(sb) + 2;
    end for;
    Console.WriteLine(acc);
    return 0;
  end Main;
end Program;
)BZL";
    // 1 + 2 + 2 + 4 + 1 + 2 = 12 per iteration.
    expectClean(runAotChecked(src), "2400\n");
}
