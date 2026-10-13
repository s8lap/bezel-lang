//! case: s20_0015
//! doc: spec
//! section: 20
//! title: a move-only branch in an owned-consuming position moves the selected binding only
//! rule: When the branches are of a **move-only value type** (§15.3), a branch
//!   naming such a binding classifies by the **position the whole expression
//!   occupies** ... in an owned-consuming position — a binding, ... — the
//!   **selected** branch's value moves (K3.12, marker-free ...), the binding
//!   joining per-path consumed after the construct (K7.4)
//! cites: 20, 15.3, 28.3
//! expect: run
//! output: chosen
//!   closed 1
//!   closed 2
//!   chosen
//!   closed 2
//!   closed 1
//! issue: 5812
module Conf;

public class Res
  public property N: int;

  public constructor(n: int)
    N := n;
  end constructor;

  destructor
    Console.WriteLine($"closed {N}");
  end destructor;
end Res;

public choice Slot
  Held(owned r: Res),
  Empty;
end Slot;

public class Demo
  public static method Run(c: bool)
    var a := Slot.Held(new Res(1));
    var b := Slot.Held(new Res(2));
    var y := (if c then a else b);
    Console.WriteLine("chosen");
  end Run;

  public static method Main()
    Run(true);
    Run(false);
  end Main;
end Demo;
