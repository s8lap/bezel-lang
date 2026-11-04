//! case: s9_4_0012
//! doc: spec
//! section: 9.4
//! title: an element store into a record's own inline-array field is rejected in a method
//! rule: The receiver's own storage is what an lvalue chain rooted at `this`
//!   reaches without leaving the value — … or an inline-array-field chain
//!   `this._arr[i] := e` (a `[N]T` field is stored inline in the record, §15).
//! cites: 15.1.2
//! expect: reject
//! diagnostic: receiver
//! notes: written in the rule's own `this._arr[i] := e` shape over an explicit
//!   field, so no property-getter copy (§23.5) gives a second ground. Twin
//!   s9_4_0020 (next free id; AUTHORING.md asks for adjacent ids, and renumbering
//!   the batch would churn every cross-reference).
module Conf;

public record Grid
  private _cells: [3]int;

  public constructor()
    _cells := [0, 0, 0];
  end constructor;

  public method Mark(i: int)
    this._cells[i] := 1;
  end Mark;
end Grid;
