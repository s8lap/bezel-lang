//! case: s35_7_0004
//! doc: spec
//! section: 35.7
//! title: co-declared equality operators on a move-only choice bind borrows
//! rule: On a move-only choice, = / <> operands and the Equals(T) parameter bind
//!   borrows — both operands stay live after the operation.
//! expect: run
//! stdout: a still fetched
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public choice Outcome
  Fetched(owned w: Widget),
  Failed;

  public static method operator =(a: Outcome, b: Outcome): bool =>
      (case a of
        Fetched(_) => (case b of Fetched(_) => true, Failed => false),
        Failed     => (case b of Failed => true, Fetched(_) => false));
  public static method operator <>(a: Outcome, b: Outcome): bool => not (a = b);
  public method Equals(other: Outcome): bool => this = other;
  public method GetHashCode(): int => (case this of Fetched(_) => 1, Failed => 0);
end Outcome;

public class Program
  public static method Main()
    using var a := Outcome.Fetched(new Widget());
    using var b := Outcome.Failed;
    Console.WriteLine(a = b);
    case a of                    // `a` is still live after the comparison
      Fetched(_) => Console.WriteLine("a still fetched"),
      Failed     => Console.WriteLine("a still failed")
    end case;
  end Main;
end Program;
