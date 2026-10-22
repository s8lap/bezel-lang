//! case: s22_8_0016
//! doc: spec
//! section: 22.8
//! title: a constructor may not compound-assign an element of a readonly array in another instance's field
//! rule: The context is per-instance, not per-type: an *instance* `readonly` field is assignable only through an lvalue chain rooted at `this` ... so a constructor of the declaring type may **not** write another value's `readonly` storage held in a parameter, local, or field.
//! cites: 22.8
//! expect: reject
//! diagnostic: readonly
module Conf;

public class Node
  public readonly Cells: [2]int;
  public owned Next: Node;

  public constructor()
    this.Next := new Node();
    this.Next.Cells[0] += 1;
  end constructor;
end Node;
