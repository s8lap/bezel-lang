//! case: s8_4_0008
//! doc: spec
//! section: 8.4
//! title: a present value compared with null inside a generic body is unequal for every type
//!   argument
//! rule: "`a = b` invokes `a.Equals(b)`, with a null-safe wrapper: `null = null` is `true`;
//!   `null = nonNull` and `nonNull = null` are `false`. `<>` is the negation." (§8.4,
//!   v0.340) Inside `G<T>`, `t = null` is the same null-safe wrapper for each instantiation,
//!   so a present record, class, overloaded-operator record or `int` is never equal to
//!   `null`.
//! cites: 14.2
//! expect: run
//! output: False
//!   False
//!   False
//!   False
module Conf;

public record Pt(X: int, Y: int);
public class C end C;
public record U(X: int)
  public static method operator =(a: U, b: U): bool => a.X = b.X;
  public static method operator <>(a: U, b: U): bool => a.X <> b.X;
  public method Equals(o: U): bool => X = o.X;
  public method GetHashCode(): int => X;
end U;

public class Program
  public static method G<T>(t: T): bool
    return t = null;
  end G;

  public static method Main()
    Console.WriteLine(G<Pt>(new Pt(1, 1)));
    Console.WriteLine(G<C>(new C()));
    Console.WriteLine(G<U>(new U(1)));
    Console.WriteLine(G<int>(1));
  end Main;
end Program;
