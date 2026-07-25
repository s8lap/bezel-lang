// RUN: %bezelc -c -o %t.o %s
//
// #2153 (§6.6 / §25.1 / §25.2, spec v0.220) — the Named-RHS reject must NOT
// fire when the RHS type declares an applicable operator overload: §25.2
// admits "a binary overload may pair the declaring type with any other type
// in either position", and the binary form `n * s` resolves through Scale's
// `operator *(int, Scale): int` and runs. The compound `n *= s` is then
// well-typed per §25.1 (the int result stores back to the int target), so
// sema must stay silent on it.
//
// #2156 (§25.2, v0.226): the codegen compound-assignment dispatch now
// resolves the operator from EITHER operand's type (matching §25.2's
// both-operand candidate set), so this program not only passes sema but
// also COMPILES cleanly (previously it aborted codegen with a malformed-IR
// error, and this lit test was a `%not` pin on the IR abort). The
// corresponding value-verifying gtest lives in
// tests/codegen/compound_assign_rhs_operator_2156_test.cpp (return value 42).

module M;
record Scale(F: int)
  public static method operator *(n: int, s: Scale): int => n * s.F;
end Scale;
class C
  public static method Main(): int
    var n: int := 6;
    var s: Scale := new Scale(7);
    n *= s;
    return n;
  end Main;
end C;
