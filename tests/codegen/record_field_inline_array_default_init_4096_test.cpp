// fix #4096 / #4507 (§15.4 "a field declaration's initializer", §9.2 body
// records, §7.9 defaults, v0.338): a record body field (or auto-property)
// of INLINE-ARRAY type whose initializer is a collection literal
// `[e1, e2, …]` used to ICE at codegen with `internal: lowerExpr returned
// null for record field/property default initializer — #1386`, because
// `Emitter::buildRecordValue`'s preamble field-init loop called the general
// `lowerExpr` on a CollectionLiteralExpr (not a general expression value).
// Control: the same literal as a local `var a: [2]int := [40, 2];` always
// compiled. Values are printed on stdout (exit codes are 8-bit truncated).
#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
std::string out(const std::string& p, bool checked = false) {
    auto r = checked ? runAotChecked(p) : runAot(p);
    if (!r.ran) return "<did not run>";
    return r.stdoutText;
}
}  // namespace

// #4096 minimal repro: no explicit constructor.
BEZEL_TEST(codegen_record_field_inline_array_default_init_4096, minimal_spec_15_4_issue_4096) {
    BEZEL_EXPECT_EQ(out(R"(module M;
record ComboArrCtl(x: int)
  public extra: [3]int := [10, 20, 30];
end ComboArrCtl;
class C
  public static method Main(): int
    var r := new ComboArrCtl(9);
    Console.WriteLine((r.x + r.extra[0] + r.extra[1] + r.extra[2]).ToString());
    return 0;
  end Main;
end C;
)", true), std::string("69\n"));
}

// #4096 second repro: a different-arity secondary constructor present.
BEZEL_TEST(codegen_record_field_inline_array_default_init_4096, with_secondary_ctor_issue_4096) {
    BEZEL_EXPECT_EQ(out(R"(module M;
record ComboArr(x: int)
  public extra: [3]int := [1, 2, 3];
  public constructor(a: int, b: int) this.x := a + b; end constructor;
end ComboArr;
class C
  public static method Main(): int
    var r := new ComboArr(9);
    var q := new ComboArr(1, 2);
    Console.WriteLine((r.x + r.extra[0] + r.extra[1] + r.extra[2]).ToString());
    Console.WriteLine((q.x + q.extra[0] + q.extra[1] + q.extra[2]).ToString());
    return 0;
  end Main;
end C;
)", true), std::string("15\n9\n"));
}

// #4507 repro: a body record with a positional field, single module.
BEZEL_TEST(codegen_record_field_inline_array_default_init_4096, issue_4507_items_field) {
    BEZEL_EXPECT_EQ(out(R"(module Solo4480;
record Arr4480(N: int)
  public Items: [2]int := [1, 2];
end Arr4480;
class App
  public static method Main(): int
    var a := new Arr4480(5);
    Console.WriteLine((a.N + a.Items[0] + a.Items[1]).ToString());
    return 0;
  end Main;
end App;
)", true), std::string("8\n"));
}

// Narrow element type (the int literal is narrowed to the byte slot), a
// string element array, and a loop so a per-construction store/leak shows.
BEZEL_TEST(codegen_record_field_inline_array_default_init_4096, narrow_and_string_elements_in_loop) {
    BEZEL_EXPECT_EQ(out(R"(module M;
record Rb(x: int)
  public b: [3]byte := [7, 8, 9];
  public s: [2]string := ["ab", "cde"];
end Rb;
class C
  public static method Main(): int
    var total := 0;
    for var i in 0..199 do
      var r := new Rb(i);
      total := total + r.b[0] + r.b[1] + r.b[2] + r.s[0].Length + r.s[1].Length;
    end;
    Console.WriteLine(total.ToString());
    return 0;
  end Main;
end C;
)", true), std::string("5800\n"));
}

// DECLINED shapes (reviewer MEASURED: the first version of the in-place arm
// compiled these into undefined behaviour). On the base each of these is
// rejected with the #1386 internal error; the arm declines them, so they are
// still a compile-time rejection with a real location, never a run.
//
// Wrong-length literal: sema has no length check for a FIELD initializer (a
// local `var a: [2]int := [1,2,3]` gets §15.4's located error), so an
// over-long literal used to write past the slot (ASan stack-buffer-overflow).
BEZEL_TEST(codegen_record_field_inline_array_default_init_4096, wrong_length_literal_declined_not_ub) {
    auto r = runAot(R"(module M;
record R(x: int)
  public a: [2]int := [1, 2, 3, 4, 5];
  public z: int := 77;
end R;
class C
  public static method Main(): int
    var r := new R(1);
    Console.WriteLine(r.z.ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "#1386"));
}

// Nullable-value elements need `{hasValue, T}` wrapping the arm does not do
// (the stored bare value read back as 0): declined, not a wrong value.
BEZEL_TEST(codegen_record_field_inline_array_default_init_4096, nullable_int_elements_declined_not_wrong_value) {
    auto r = runAot(R"(module M;
record R(x: int)
  public ni: [2]int? := [5, null];
end R;
class C
  public static method Main(): int
    var r := new R(1);
    Console.WriteLine((r.ni[0] ?? -1).ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "#1386"));
}

BEZEL_TEST(codegen_record_field_inline_array_default_init_4096, nullable_record_elements_declined_not_wrong_value) {
    auto r = runAot(R"(module M;
record Pt(a: int, b: int) end Pt;
record R(x: int)
  public np: [2]Pt? := [new Pt(1, 2), null];
end R;
class C
  public static method Main(): int
    var r := new R(1);
    Console.WriteLine((r.np[0]?.b ?? -1).ToString());
    return 0;
  end Main;
end C;
)");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "#1386"));
}
