//! case: l2_18_0014
//! doc: stdlib
//! section: 2.18
//! title: a user-defined IFormattable defines its own specifier vocabulary
//! rule: stdlib §2.18: "A **user-defined** `IFormattable` implementation defines
//!     its own specifier vocabulary — the table below governs only the
//!     intrinsic types ... (`money` above is such a user type defining its own
//!     `"C2"`; the intrinsic vocabulary deliberately has no `C`)". The
//!     interpolation hole dispatches the spec to the user's own `ToString`.
//! cites: 2.18, 18.5
//! expect: run
//! stdout: EUR C2
module Conf;

public class Money: IFormattable
  public method ToString(format: string): string
    return "EUR " + format;
  end ToString;
end Money;

public class Program
  public static method Main()
    var m := new Money();
    Console.WriteLine($"{m:C2}");
  end Main;
end Program;
