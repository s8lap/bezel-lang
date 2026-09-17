//! case: s1_6_0006
//! doc: spec
//! section: 1.6
//! title: a labeled loop closed with its own label is accepted
//! rule: A loop carrying a loop label is a named construct; its closer is
//!   `end label;` or the unlabelled `end;`.
//! expect: accept
module Conf;

public class Holder
  private method Scan()
    scan: for var i in 1..3 do
      Console.WriteLine($"{i}");
    end scan;
  end Scan;
end Holder;
