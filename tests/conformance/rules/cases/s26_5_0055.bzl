//! case: s26_5_0055
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a task-typed field is rejected
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules
//! cites: 26.5, 30.3
//! expect: reject
//! diagnostic: Bogus
module Conf;

public class Holder
  public Cell: Task<int>? := null;

  public method Poke()
    var c := this.Cell;
    if c <> null then
      c.Bogus();
    end;
  end Poke;
end Holder;
