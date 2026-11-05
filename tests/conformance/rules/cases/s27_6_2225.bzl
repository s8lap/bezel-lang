//! case: s27_6_2225
//! doc: spec
//! section: 27.6
//! title: generic overloads of two arities beside one non-generic: the
//!   untargeted reference forms Len()
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 42
//! notes: Trace: both generic overloads are excluded, leaving Len(), so the
//!   untargeted reference forms () -> int, f() = 42. The pre-fix
//!   compiler (79accd22) rejects this program because it counts the
//!   generic overload as a candidate; it passes on the staging tip.
module Conf;

class V
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(): int => 42;
  public method Len<T, U>(x: T, y: U): int => 8;
end V;

public class Program
  public static method Main()
    var v := new V();
    var f := v.Len;
    Console.WriteLine(f());
  end Main;
end Program;
