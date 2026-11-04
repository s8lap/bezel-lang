//! case: s9_4_0012
//! doc: spec
//! section: 9.4
//! title: an element store into a record's own inline-array property is rejected in a method
//! rule: The receiver's own storage is what an lvalue chain rooted at `this`
//!   reaches without leaving the value — … or an inline-array-field chain
//!   `this._arr[i] := e` (a `[N]T` field is stored inline in the record, §15).
//! cites: 15.1.2
//! expect: reject
//! diagnostic: receiver
module Conf;

public record Grid
  public property Cells: [3]int;

  public constructor()
    Cells := [0, 0, 0];
  end constructor;

  public method Mark(i: int)
    Cells[i] := 1;
  end Mark;
end Grid;
