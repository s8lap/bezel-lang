//! case: s26_5_0055
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on a task-typed field is rejected
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type.
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
