//! case: l3_1_0052
//! doc: stdlib
//! section: 3.1
//! title: an Array<int> argument to AppendFormat's Array<Object> parameter is rejected
//! rule: stdlib §3.1 declares `AppendFormat(format: string, args: Array<Object>): StringBuilder`.
//!   §12.9: classes, records, and concrete generic types are always invariant, so
//!   `Array<int>` is not `Array<Object>`; §5.1's reference conversions admit no
//!   such assignment, and §22.6 finds no applicable candidate.
//! cites: 3.1, 12.9, 5.1, 22.6
//! expect: reject
//! diagnostic: Array<Object>
//! issue: 7207
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    var args: Array<int> := [1];
    _ := sb.AppendFormat("{0}", args);
  end Main;
end Program;
