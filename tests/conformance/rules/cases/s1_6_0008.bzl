//! case: s1_6_0008
//! doc: spec
//! section: 1.6
//! title: a nested loop with a distinct label is accepted
//! rule: A loop label duplicates no loop label in scope at its declaration;
//!   a distinct inner label is unaffected.
//! expect: accept
module Conf;

public class Holder
  private method Nested()
    scan: for var i in 1..3 do
      inner: for var j in 1..3 do
        Console.WriteLine($"{j}");
      end inner;
    end scan;
  end Nested;
end Holder;
