//! case: s28_3_7742
//! doc: spec
//! section: 28.3
//! title: a getter's self-assignment after a loop that may run zero times is rejected
//! rule: A self-assignment is no exception: for an owned-state class-typed local `t`, `t := t;` stores the unmarked read `t`, a borrow, into an owned-state LHS and is rejected at the assignment by this rule
//! cites: 28.9
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
      var k := 1;
      while k < 3 do
        case k of 1 => t := null, _ => t := null end case;
        k := k + 1;
      end;
      t := t;
      return null;
    end;
  end Item;
end H;
