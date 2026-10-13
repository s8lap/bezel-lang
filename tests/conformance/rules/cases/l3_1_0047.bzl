//! case: l3_1_0047
//! doc: stdlib
//! section: 3.1
//! title: a StringBuilder's Length reads its character count
//! rule: `Length` measures a fixed or contiguous extent — text (`String`, `StringBuilder`, `ReadOnlyStringSpan`) (stdlib §1, Count vs Length); StringBuilder declares `Length: int`.
//!   A StringBuilder holding the three characters abc has a Length of 3.
//! cites: 1, 3.1
//! expect: run
//! stdout: 3
//! issue: 6081
module Conf;

public class C
  public static method Main(): int
    var sb := new StringBuilder();
    sb.Append("abc");
    Console.WriteLine(sb.Length);
    return 0;
  end Main;
end C;
