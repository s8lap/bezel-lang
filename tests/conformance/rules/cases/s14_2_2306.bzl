//! case: s14_2_2306
//! doc: spec
//! section: 14.2
//! title: Math.Max over a user record named IntPtr that implements
//!   IComparable is accepted
//! rule: Non-nullable types conforming to `IFoo`: classes implementing it,
//!   records and enums implementing it, interfaces extending it
//! cites: 2.2, 11.2
//! expect: run
//! output: 5
//! notes: Accepting twin of the IntPtr rejects: a user record that merely
//!   shares the NAME of the stdlib IntPtr but implements IComparable
//!   satisfies the constraint, so the rejection is by conformance, not by
//!   name.
module Conf;

public record IntPtr(X: int): IComparable<IntPtr>
  public method CompareTo(other: IntPtr): int
    return X - other.X;
  end CompareTo;
end IntPtr;

public class Program
  public static method Main()
    var a := new IntPtr(1);
    var b := new IntPtr(5);
    Console.WriteLine(Math.Max(a, b).X);
  end Main;
end Program;
