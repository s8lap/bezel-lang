//! case: l9_6_9901
//! doc: stdlib
//! section: 9.6
//! title: assigning the read-only Length of a string local is rejected
//! rule: public pure readonly property Length: int;
//! cites: 9.6, 23.1
//! expect: reject
//! diagnostic: read-only
//! issue: 9250
//! notes: Harvested from the #9250 review (rej_string_assign). String.Length is declared `readonly` with
//!   no setter, so the assignment has no target to write.
module Conf;

public class Program
  public static method Main()
    var t := "abc";
    t.Length := 3;
  end Main;
end Program;
