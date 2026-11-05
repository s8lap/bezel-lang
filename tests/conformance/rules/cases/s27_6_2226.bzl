//! case: s27_6_2226
//! doc: spec
//! section: 27.6
//! title: a static method group with a generic overload first: the
//!   untargeted reference forms Len()
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 42
//! notes: Trace: the static generic overload is excluded, leaving Len(),
//!   f() = 42. The pre-fix compiler (79accd22) rejects this program
//!   because it counts the generic overload as a candidate; it passes
//!   on the staging tip.
module Conf;

class U
  public static method Len<T>(x: T): int => 7;
  public static method Len(): int => 42;
end U;

public class Program
  public static method Main()
    var f := U.Len;
    Console.WriteLine(f());
  end Main;
end Program;
