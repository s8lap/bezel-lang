//! case: s1_6_0007
//! doc: spec
//! section: 1.6
//! title: a nested loop cannot reuse an enclosing loop's label
//! rule: A loop label duplicates no loop label in scope at its declaration
//!   (Appendix C.5).
//! cites: 1.6, 21.5, C.5
//! expect: reject
//! diagnostic: duplicate
module Conf;

public class Holder
  private method Nested()
    scan: for var i in 1..3 do
      scan: for var j in 1..3 do
        Console.WriteLine($"{j}");
      end scan;
    end scan;
  end Nested;
end Holder;
