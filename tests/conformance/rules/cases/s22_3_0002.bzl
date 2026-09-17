//! case: s22_3_0002
//! doc: spec
//! section: 22.3
//! title: the caller must write ref at the call site
//! rule: "`ref` — pass by reference. The caller writes `ref` at the call
//!   site."
//! cites: 22.3
//! expect: reject
//! diagnostic: ref
module Conf;

public class Program
  public static method Increment(ref n: int)
    n := n + 1;
  end Increment;

  public static method Main()
    var x := 1;
    Increment(x);
  end Main;
end Program;
