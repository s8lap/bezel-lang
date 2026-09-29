var b: byte := 250;
b += 4;                    // b := byte(b + 4) — 254, in range
b += 10;                   // byte(264) → OverflowException at runtime, never a wraparound
var checksum: byte := 0;
checksum ^= b;             // byte ^ byte : byte — R = T, no write-back involved
var n: int := 2;
var acc: byte := 40;
acc += n;                  // admitted: R = int; checked write-back at runtime (42)
n mod= 3;                  // n := n mod 3 — 2; a zero divisor raises DivideByZeroException (§6.1)
var s: short := 1;
s <<= 2;                   // R = short (§6.5) — 4, no write-back involved
var c: char := 'a';
c += 1;                    // c := char(int(c) + 1) — 'b'; §5.2's code-point check applies
var m: int? := 0;
m += 1;                    // admitted: the initializer narrowed m non-null (§7.4) — 1
m := null;                 // reassigned from null, a nullable-typed expression: the narrowing ends (§7.4)
// m += 1;                 // compile error: `+` is not defined on a nullable operand
//                         //   (§7.11) — write m := (m ?? 0) + 1; or narrow first
// m++;                    // compile error: the same rule through §17.6's desugaring
counts[category] += 1;     // §17.6's indexer instance: collection, index, getter, setter — once each
// var f: float := 1.0f;
// f += 2.5;               // compile error: R = double does not return to float implicitly —
//                         //   write f += 2.5f; (float promotion stays in-family) or
//                         //   f := float(f + 2.5); if the double-rounding is intended
