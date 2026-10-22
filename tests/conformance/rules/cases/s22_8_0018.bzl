//! case: s22_8_0018
//! doc: spec
//! section: 22.8
//! title: a constructor may write an element of its own readonly inline array field
//! rule: The context is per-instance, not per-type: an *instance* `readonly` field is assignable only through an lvalue chain rooted at `this` ... so a constructor of the declaring type may **not** write another value's `readonly` storage held in a parameter, local, or field.
//! cites: 22.8
//! expect: accept
module Conf;

public class Board
  public readonly Cells: [2]int;

  public constructor()
    this.Cells[0] := 1;
  end constructor;
end Board;
