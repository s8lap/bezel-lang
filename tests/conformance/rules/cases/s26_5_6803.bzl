//! case: s26_5_6803
//! doc: spec
//! section: 26.5
//! title: the `?.` call spelling of the property IsReadOnly through a nullable IList receiver is refused
//! rule: the call is a compile error citing the instance member(s); it does not fall back to an extension
//! cites: 26.5, 7.5, 27.7, 4.1
//! expect: reject
//! diagnostic: instance member
//! issue: 8650
//! notes: The `?.` spelling is the `.` spelling of 26.5 plus 7.5's null guard (`x?.M` ... otherwise it performs the access),
//!   so it is refused as `c.IsReadOnly()` is (s26_5_6801).
module Conf;

public class Program
  public static method Main()
    var a: Array<int> := [1, 2];
    var c: IList<int>? := a;
    var b := c?.IsReadOnly();
  end Main;
end Program;
