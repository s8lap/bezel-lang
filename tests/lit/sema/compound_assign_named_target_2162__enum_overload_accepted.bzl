// RUN: %bezelc -c -o %t.o %s
//
// Issue #2162 over-rejection guard (§6.6/§25.2/§10.4, spec v0.226) — the
// Named-TARGET reject must NOT fire when the target's enum declares an
// applicable `operator <op>` overload: §10.4 admits operator overloads on
// enums, §25.2 draws the candidate set from the operand's static type, and
// §6.6's desugar `x := x op e` then resolves through the user overload
// (§25.1). `p |= Priority.High` and `p += 2` both resolve through the
// declared operators, so sema must stay silent. Pins that the fix does not
// over-reject the legitimate enum-compound-with-overload surface.

module M;
enum Priority Low, Medium, High;
  public static method operator |(a: Priority, b: Priority): Priority
    => Priority(int(a) | int(b));
  public static method operator +(a: Priority, b: int): Priority
    => Priority(int(a) + b);
end Priority;
class C public static method Main(): int
  var p: Priority := Priority.Low;
  p |= Priority.High;
  p += 0;
  return int(p);
end Main; end C;
