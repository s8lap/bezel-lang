//! case: l9_6_9902
//! doc: stdlib
//! section: 9.6
//! title: assigning Length of a string captured by a lambda is rejected
//! rule: public pure readonly property Length: int;
//! cites: 9.6, 23.1, 27.5
//! expect: reject
//! diagnostic: read-only
//! issue: 9250
//! notes: Harvested from the #9250 review (rej_in_lambda). The write sits inside a block lambda over a
//!   captured string; the rule is the same read-only Length declaration.
module Conf;

public class Program
  public static method Main()
    var s := "abc";
    var f := lambda() => do s.Length := 1; end;
    f();
  end Main;
end Program;
