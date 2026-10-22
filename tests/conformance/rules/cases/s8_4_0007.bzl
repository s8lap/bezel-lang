//! case: s8_4_0007
//! doc: spec
//! section: 8.4
//! title: comparing a record with null does not dispatch to its overloaded equality operator
//! rule: "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper: `null = null` is `true`;
//!   `null = nonNull` and `nonNull = null` are `false`. `<>` is the negation." (§8.4,
//!   v0.340) The wrapper "belongs to the equality operators, not to `Equals`: ... only when
//!   both operands are present dispatch to the overloaded operator where one is declared"
//!   (§8.4). With `null` as one operand the operator body never runs, so none of its marker
//!   lines is printed before the four results.
//! cites: 25.2
//! expect: run
//! output: False
//!   True
//!   False
//!   True
module Conf;

public record U(X: int)
  public static method Say(m: string, v: bool): bool
    Console.WriteLine(m);
    return v;
  end Say;

  public static method operator =(a: U, b: U): bool => Say("user=", a.X = b.X);
  public static method operator <>(a: U, b: U): bool => Say("user<>", a.X <> b.X);
  public method Equals(o: U): bool => X = o.X;
  public method GetHashCode(): int => X;
end U;

public class Program
  public static method Main()
    var u := new U(1);
    Console.WriteLine(u = null);
    Console.WriteLine(u <> null);
    Console.WriteLine(null = u);
    Console.WriteLine(null <> u);
  end Main;
end Program;
