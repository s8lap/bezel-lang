//! case: l2_18_4504
//! doc: stdlib
//! section: 2.18
//! title: a run of a specifier letter at any other length than its own raises
//!   FormatException
//! rule: a run of one of the specifier letters at any other length is the malformed
//!   run the paragraph above faults.
//! cites: 2.5
//! expect: run
//! output: 2
//! issue: 2240
module Conf;

public class C
  public static method Main()
    var n := new TimeOnly(23, 59, 59, 999);
    var r := 0;
    try
      Console.WriteLine(n.ToString("HHH"));
    catch FormatException => r := r + 1;
    end;
    try
      Console.WriteLine(n.ToString("s"));
    catch FormatException => r := r + 1;
    end;
    Console.WriteLine(r.ToString());
  end Main;
end C;
