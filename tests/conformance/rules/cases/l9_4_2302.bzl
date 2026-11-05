//! case: l9_4_2302
//! doc: stdlib
//! section: 9.4
//! title: Math.Min over bool is refused
//! rule: public record Boolean: IEquatable<bool>
//! cites: 2.2, 14.2, 3.5
//! expect: reject
//! diagnostic: constraint
//! issue: 8287
//! notes: Red on base: the pre-fix Math special case bypassed the ordinary
//!   constraint check and admitted this program, the staging tip reports
//!   the constraint failure.
module Conf;

public class Program
  public static method Main()
    var m := Math.Min(true, false);
    Console.WriteLine(m);
  end Main;
end Program;
