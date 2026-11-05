//! case: s27_6_2227
//! doc: spec
//! section: 27.6
//! title: an interface method group with a generic member first: the
//!   untargeted reference forms Len()
//! rule: without one, only candidates declaring none take part, and the
//!   error above is reported where every candidate is generic
//! cites: 27.6, 14.5
//! expect: run
//! stdout: 42
//! notes: Trace: the interface member Len<T> is excluded, leaving Len(),
//!   the call dispatches to the V implementation, f() = 42. The
//!   pre-fix compiler (79accd22) rejects this program because it
//!   counts the generic overload as a candidate; it passes on the
//!   staging tip.
module Conf;

interface I
  method Len<T>(x: T): int;
  method Len(): int;
end I;
class V : I
  public constructor() end constructor;
  public method Len<T>(x: T): int => 7;
  public method Len(): int => 42;
end V;

public class Program
  public static method Main()
    var i: I := new V();
    var f := i.Len;
    Console.WriteLine(f());
  end Main;
end Program;
