//! case: s12_7_0002
//! doc: spec
//! section: 12.7
//! title: a chain-extending store into a value-narrowed Object box is rejected
//! rule: A write whose store would land in the box is a compile error: an
//!   assignment, compound assignment, or ++/-- whose target is an lvalue
//!   chain extending the binding with a . or [...] hop.
//! cites: 12.7
//! expect: reject
//! diagnostic: box
module Conf;

public class Program
  public static method Test()
    var a: [3]int := [4, 5, 6];
    var o: Object := a;
    if o is [3]int then
      o[0] := 9;
    end if;
  end Test;
end Program;
