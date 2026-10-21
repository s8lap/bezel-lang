//! case: s9_2_0033
//! doc: spec
//! section: 9.2
//! title: a record's Equals may not take Object? because its Object slot is own-typed
//! rule: "On every value kind the `Equals` is the one taking the kind's own type" ([§9.2]), and "no value
//!   kind declares an `Equals` at `Object?`" ([§9.2]). The record declares `public method Equals(o:
//!   Object?): bool => true;` on line 7, which is not its own-typed `Equals`, so the compiler rejects the
//!   declaration. The second diagnostic (the `hashable` requirement of `Contains`) follows from the same
//!   record and is not needed for the reject.
//! cites: 9.2
//! expect: reject
//! diagnostic: record member `Equals` may not take `Object?`
//! issue: 6665
module Conf;
public record B
  public readonly property X: int;
  public constructor(x: int)
    X := x;
  end constructor;
  public method Equals(o: Object?): bool => true;
  public method GetHashCode(): int => 0;
end B;
public class Program
  public static method Main()
    using var a := new Array<B>();
    a.Add(new B(5));
    Console.WriteLine(a.Contains(new B(6)));
  end Main;
end Program;
