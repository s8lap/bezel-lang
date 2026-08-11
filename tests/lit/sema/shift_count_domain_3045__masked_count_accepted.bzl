// §6.5 (v0.242, proposal 0079 / #3045), the prescribed migration spelling:
// "Code that wants the modular count deliberately writes it: `v << (k & 31)`
// (`& 63` for 64-bit left operands) — the mask is then in the source, visible
// and in-domain by construction. (§17.2's mandatory-parenthesization rules do
// not cover shift-with-bitwise mixes, but `&` binds looser than `<<`, so the
// parentheses around `k & 31` are required by precedence — the spelling is
// unambiguous.)"
//
// Every arm below is in-domain by construction and must compile cleanly: the
// runtime spelling, the 64-bit `& 63` form, the byte `& 7` form, the compound
// form, and a CONSTANT masked count in an inline-array size.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
class C
  public static method Main(): int
    var k: int := 40;
    var v: int := 1;
    var a := v << (k & 31);
    var l: long := 1L;
    var b := l << (k & 63);
    var y: byte := 1;
    var c: byte := y << (k & 7);
    v <<= (k & 31);
    var arr: [1 << (40 & 31)]int;
    return (if a > 0 and b > 0L and int(c) > 0 and v > 0 and arr.Length = 256 then 0 else 1);
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
