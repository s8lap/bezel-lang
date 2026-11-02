//! case: s28_11_0047
//! doc: spec
//! section: 28.11
//! title: move of a slot of a plain int inline array is rejected
//! rule: A copy, shared-reference or borrow slot, and a read-only view, admit none
//! cites: 28.11, 15.1
//! expect: reject
//! diagnostic: slot
module Conf;

public class Program
  public static method Main(): int
    var a: [2]int := [1, 2];
    using var r := move a[0];
    return 0;
  end Main;
end Program;
