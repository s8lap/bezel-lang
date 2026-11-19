//! case: s28_3_7741
//! doc: spec
//! section: 28.3
//! title: a getter's self-assignment after a non-exhaustive case that nulls an owned local is rejected
//! rule: A self-assignment is no exception: for an owned-state class-typed local `t`, `t := t;` stores the unmarked read `t`, a borrow, into an owned-state LHS and is rejected at the assignment by this rule
//! cites: 19.6, 28.9
//! expect: reject
//! diagnostic: self-assign
module Conf;
public enum Color
  Red, Green, Blue;
end Color;
public class Cell
  public value: int := 0;
  public constructor(x: int) this.value := x; end constructor;
end Cell;
public class H
  public property Item: Cell?
    get => do
      var t: Cell? := new Cell(1);
      var c := Color.Green;
      case c of Color.Red => t := null, Color.Green => t := null end case;
      t := t;
      return null;
    end;
  end Item;
end H;
