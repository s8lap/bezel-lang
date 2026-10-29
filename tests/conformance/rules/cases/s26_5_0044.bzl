//! case: s26_5_0044
//! doc: spec
//! section: 26.5
//! title: a null-conditional call reaches an extension for an undeclared name, a member for a declared one
//! rule: instance resolution is committed and extension blocks are **never consulted** for that name on that receiver ... `x?.M` returns null if `x` is null; otherwise it performs the access. The result type is always nullable
//! cites: 26.5, 7.5
//! expect: run
//! output: True
//!   3
//!   False
//!   -1
module Conf;

class Box
  public method Contains(x: int): bool => true;
end Box;

extension Box
  public method Frob(): int => 3;
end;

public class Program
  public static method Probe(b: Box?)
    Console.WriteLine(b?.Contains(1) ?? false);
    Console.WriteLine(b?.Frob() ?? -1);
  end Probe;

  public static method Main()
    var bx := new Box();
    Probe(bx);
    Probe(null);
  end Main;
end Program;
