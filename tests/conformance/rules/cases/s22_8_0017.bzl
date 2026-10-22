//! case: s22_8_0017
//! doc: spec
//! section: 22.8
//! title: a constructor may not write a readonly inline-array element through a local of another instance
//! rule: The context is per-instance, not per-type: an *instance* `readonly` field is assignable only through an lvalue chain rooted at `this` ... so a constructor of the declaring type may **not** write another value's `readonly` storage held in a parameter, local, or field.
//! cites: 22.8
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Node
  public readonly Cells: [2]int;

  public constructor()
    var o := new Node();
    o.Cells[1] := 4;
  end constructor;
end Node;
