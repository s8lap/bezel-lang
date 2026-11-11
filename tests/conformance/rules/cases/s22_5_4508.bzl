//! case: s22_5_4508
//! doc: spec
//! section: 22.5
//! title: omitting the required condition parameter of Debug.Assert while naming
//!   message is a compile error
//! rule: Every parameter that declares no default must be bound
//! cites: 22.6
//! expect: reject
//! diagnostic: condition
//! issue: 7602
module Conf;
import System.Diagnostics;

class C
  public static method Main(): int
    Debug.Assert(message := "m");
    return 0;
  end Main;
end C;
