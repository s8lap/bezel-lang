//! case: l3_1_9901
//! doc: stdlib
//! section: 3.1
//! title: a compound assignment to StringBuilder.Length is rejected
//! rule: public readonly property Length: int;
//! cites: 3.1, 6.6, 23.1
//! expect: reject
//! diagnostic: read-only
//! issue: 9250
//! notes: Harvested from the #9250 review (rej_sb_minus). `sb.Length -= 1` expands to `sb.Length :=
//!   sb.Length - 1` (6.6), whose target is the get-only property.
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    sb.Length -= 1;
  end Main;
end Program;
