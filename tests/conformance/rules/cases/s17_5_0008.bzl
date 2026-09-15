//! case: s17_5_0008
//! doc: spec
//! section: 17.5
//! title: an inline array's intrinsic Contains is reachable through in
//! rule: "Inline arrays expose an intrinsic `Contains` (§15.1.2), so `x in
//!   inlineArr` is admitted through this section's structural lookup like
//!   any other collection — no special case."
//! cites: 17.5, 15.1.2
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var arr: [3]int := [1, 2, 3];
    var r := 2 in arr;
    Console.WriteLine(r);
  end Main;
end Program;
