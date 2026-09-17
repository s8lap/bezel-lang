//! case: s22_3_0001
//! doc: spec
//! section: 22.3
//! title: a ref local argument is a true alias, visible through the caller's location
//! rule: "`ref` local and field targets alias: callee writes are visible
//!   through the caller's location immediately (the `Swap(ref a, ref b)`
//!   semantics presupposes this)."
//! cites: 22.3
//! expect: run
//! stdout: 2
module Conf;

public class Program
  public static method Swap(ref a: int, ref b: int)
    var t := a;
    a := b;
    b := t;
  end Swap;

  public static method Main()
    var x := 1;
    var y := 2;
    Swap(ref x, ref y);
    Console.WriteLine(x);
  end Main;
end Program;
