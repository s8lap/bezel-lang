// RUN: %bezelc -c -o %t.o %s
//
// spec §4.2/§4.3, v0.190 (#1781): the formation range-check must NOT over-reject
// valid literals. This accept case pins, in one compile:
//   - a large-but-valid `double` (1.5e300, well under double.MaxValue ~1.8e308)
//   - a `float` near its largest finite value (3.0e38f, under ~3.4e38)
//   - the `decimal.MaxValue` literal (79228162514264337593543950335m — exactly
//     representable, must stay accepted)
//   - the §4.2 boundary carve-out `-9223372036854775808L` (long.MinValue, the
//     magnitude 2^63 admitted only as the immediate operand of unary `-`)
//   - hex- and binary-spelled suffixed integer literals at their type maxima
//     (0xFFFFFFFFU = uint.MaxValue, 0b11111111 fits int)
module M;
class C
  public static method Main(): int
    var d: double := 1.5e300;
    var f: float := 3.0e38f;
    var m: decimal := 79228162514264337593543950335m;
    var lmin: long := -9223372036854775808L;
    var hx: uint := 0xFFFFFFFFU;
    var bn: int := 0b11111111;
    return 0;
  end Main;
end C;
