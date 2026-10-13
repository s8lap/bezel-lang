//! case: s20_0016
//! doc: spec
//! section: 20
//! title: the move marker on a move-only branch is rejected
//! rule: in an owned-consuming position ... the **selected** branch's value
//!   moves (K3.12, marker-free; the marker is rejected there as at every
//!   move-only position)
//! cites: 20, 15.3, 28.3
//! expect: reject
//! diagnostic: move
module Conf;

public class Res
  public constructor()
  end constructor;
end Res;

public choice Slot
  Held(owned r: Res),
  Empty;
end Slot;

public class Demo
  public static method Run(c: bool)
    var a := Slot.Held(new Res());
    var b := Slot.Held(new Res());
    var y := (if c then move a else move b);
  end Run;
end Demo;
