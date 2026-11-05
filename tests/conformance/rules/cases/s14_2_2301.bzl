//! case: s14_2_2301
//! doc: spec
//! section: 14.2
//! title: Math.Max over IntPtr is refused, IntPtr declares no IComparable
//! rule: Non-nullable types conforming to `IFoo`: classes implementing it,
//!   records and enums implementing it, interfaces extending it
//! cites: 2.2, 10.0
//! expect: reject
//! diagnostic: constraint
//! issue: 8287
//! notes: Red on base: the pre-fix Math special case bypassed the ordinary
//!   constraint check and admitted this program, the staging tip reports
//!   the constraint failure.
module Conf;

public class Program
  public static method Main()
    var m := Math.Max(IntPtr.Zero, IntPtr.Zero);
  end Main;
end Program;
