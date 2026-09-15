//! case: s1_6_0005
//! doc: spec
//! section: 1.6
//! title: a labeled loop closed with the unlabeled keyword form is rejected
//! rule: A loop carrying a loop label is a named construct; the keyword
//!   closer (`end for;`) is rejected once the loop is labeled.
//! cites: 1.6, C.5
//! expect: reject
//! diagnostic: label
module Conf;

public class Holder
  private method Scan()
    scan: for var i in 1..3 do
      Console.WriteLine($"{i}");
    end for;
  end Scan;
end Holder;
