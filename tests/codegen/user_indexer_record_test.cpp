// Codegen — a USER-DEFINED indexer (§23.4) whose result is a RECORD value (§9).
//
// A record is a by-value LLVM struct (§9.4); a user indexer returns a value
// COPY (records are value types — §9, §23.5: "a value-typed indexer result" is
// a discarded copy). The user-indexer get lowering (LlvmBackend_Assign.cpp
// `lowerIndexerGet`) emits the indexer's getter and materializes its result.
//
// EXPRESSION-bodied getters (`property [i]: Rec => …` and `get => …; end;`)
// already lowered correctly. This file pins that, AND covers the BLOCK-form
// getter (`get => do … return …; end; end;`, §23.4): the block getter contains
// a `return`, which — when the getter was lowered by INLINING into the caller —
// terminated the *caller* (a record `return` against an `int`-returning Main),
// crashing/mis-returning. The fix emits the block-form indexer getter as a
// standalone function (mirroring the block-form *property* getter, §22.9), so
// `return` exits only the getter and the record result flows back by value.
//
// Controls guard that the fix does not perturb the working paths: a scalar
// (int) user indexer, a record-returning METHOD, and a built-in collection
// indexer (Array<int>[i]) all still produce correct values.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {
int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}
} // namespace

// ---------------------------------------------------------------------------
// Expression-bodied user indexer returning a record (§23.4 / §9.4).
// ---------------------------------------------------------------------------

// §23.4: `property [i: int]: Rec => new Rec(...)` — bind the result to a local
// and read EACH field. Load-bearing: both fields must be the constructed values
// (not 0) — a record result mishandled as a scalar/pointer would zero a field.
BEZEL_TEST(user_indexer_record_23_4, expr_getter_bind_then_read_both_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C "
        "  public property [i: int]: Pair => new Pair(i + 1, i + 2); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c[5]; "
        "    return v.A * 1000 + v.B; "          // 6*1000 + 7 = 6007
        "  end Main; end C;"), 6007);
}

// §23.4: field access DIRECTLY on the indexer result (`c[5].A`, `c[5].B`) — the
// record materializes per access; each field is correct.
BEZEL_TEST(user_indexer_record_23_4, expr_getter_direct_field_access) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C "
        "  public property [i: int]: Pair => new Pair(i + 1, i + 2); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c[5].A * 1000 + c[5].B; "    // 6007
        "  end Main; end C;"), 6007);
}

// §23.4 / §9.4: the indexer reads a backing RECORD FIELD and returns a copy of
// it; both fields of the copy are correct.
BEZEL_TEST(user_indexer_record_23_4, expr_getter_returns_backing_record_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C "
        "  private _p: Pair; "
        "  public constructor() _p := new Pair(60, 7); end constructor; "
        "  public property [i: int]: Pair => _p; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c[0]; "
        "    return v.A * 1000 + v.B; "          // 60007
        "  end Main; end C;"), 60007);
}

// §23.4 / §9.4: a wider record (4 int fields, sret-class size) returns all
// fields correctly through the user-indexer get.
BEZEL_TEST(user_indexer_record_23_4, expr_getter_wide_record_all_fields) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Quad(A: int, B: int, C: int, D: int); "
        "class C "
        "  public property [i: int]: Quad => new Quad(i + 1, i + 2, i + 3, i + 4); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c[5]; "
        "    return v.A * 1000 + v.B * 100 + v.C * 10 + v.D; "   // 6789
        "  end Main; end C;"), 6789);
}

// §23.4 / §9.2 / §3: a record with a string field round-trips a pointer + int
// pair through the user-indexer get; both the string and int field are correct.
BEZEL_TEST(user_indexer_record_23_4, expr_getter_record_with_string_field) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Named(Name: string, Age: int); "
        "class C "
        "  public property [i: int]: Named => new Named(\"hello\", i + 1); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c[5]; "
        "    return v.Name.Length * 1000 + v.Age; "   // 5*1000 + 6 = 5006
        "  end Main; end C;"), 5006);
}

// ---------------------------------------------------------------------------
// Block-form user indexer returning a record (§23.4 `get => do … end;`).
// The block body contains a `return`; the getter must be a standalone function
// so the `return` exits only the getter (not the int-returning Main).
// ---------------------------------------------------------------------------

// §23.4 / §22.9: block-form getter `get => do return new Pair(...); end;` —
// bound to a local; each field correct. Before the fix the inlined block `return`
// terminated Main with a record value against an int return → crash/garbage.
BEZEL_TEST(user_indexer_record_23_4, block_getter_record_bind_then_read) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C "
        "  public property [i: int]: Pair "
        "    get => do return new Pair(i + 1, i + 2); end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c[5]; "
        "    return v.A * 1000 + v.B; "          // 6007
        "  end Main; end C;"), 6007);
}

// §23.4 / §22.9: a block-form getter with intermediate statements (a local then
// a `return` of it) returning a record — all fields correct.
BEZEL_TEST(user_indexer_record_23_4, block_getter_record_local_then_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C "
        "  public property [i: int]: Pair "
        "    get => do "
        "      var p := new Pair(i + 10, i + 20); "
        "      return p; "
        "    end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c[5]; "
        "    return v.A * 1000 + v.B; "          // 15*1000 + 25 = 15025
        "  end Main; end C;"), 15025);
}

// §23.4 / §22.9: a block-form getter with control flow (the `return` is inside
// an `if`) returning a record — the standalone getter terminates correctly and
// the chosen record is returned.
BEZEL_TEST(user_indexer_record_23_4, block_getter_record_conditional_return) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C "
        "  public property [i: int]: Pair "
        "    get => do "
        "      if i > 0 then return new Pair(7, 8); end; "
        "      return new Pair(1, 2); "
        "    end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c[3]; "
        "    return v.A * 1000 + v.B; "          // 7008
        "  end Main; end C;"), 7008);
}

// ---------------------------------------------------------------------------
// Controls — the fix must not perturb these working paths.
// ---------------------------------------------------------------------------

// CONTROL (§23.4): a SCALAR (int) user indexer still returns the right value
// (the value-typed scalar indexer path is unaffected by the record fix).
BEZEL_TEST(user_indexer_record_23_4, control_scalar_indexer_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property [i: int]: int => i * 2; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c[21]; "                     // 42
        "  end Main; end C;"), 42);
}

// CONTROL (§23.4 / §22.9): a SCALAR (int) BLOCK-form user indexer still returns
// the right value — the standalone-getter fix applies to the block form for any
// result type, scalar included.
BEZEL_TEST(user_indexer_record_23_4, control_scalar_block_getter_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C "
        "  public property [i: int]: int "
        "    get => do return i * 2; end; "
        "  end; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    return c[21]; "                     // 42
        "  end Main; end C;"), 42);
}

// CONTROL (§9.4 / §22): a record-returning METHOD still works (the path the
// indexer fix is modeled on).
BEZEL_TEST(user_indexer_record_23_4, control_record_returning_method_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; record Pair(A: int, B: int); "
        "class C "
        "  public method GetP(i: int): Pair => new Pair(i + 1, i + 2); "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    var v := c.GetP(5); "
        "    return v.A * 1000 + v.B; "          // 6007
        "  end Main; end C;"), 6007);
}

// CONTROL (§4.4): a built-in COLLECTION indexer (Array<int>[i]) still reads the
// right element (the collection indexer path is distinct from the user-indexer
// path and must remain unaffected).
BEZEL_TEST(user_indexer_record_23_4, control_collection_indexer_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var a := new Array<int>(); "
        "  a.Add(10); a.Add(20); a.Add(30); "
        "  return a[1] + 0; "                    // 20
        "end Main; end C;"), 20);
}
