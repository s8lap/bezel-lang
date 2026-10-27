//! case: s12_7_0046
//! doc: spec
//! section: 12.7
//! title: a narrowed nullable Guid renders its payload's D form, not the carrier
//! rule: `is null` tests whether the value is null. `is not null` tests whether it
//!   is non-null and narrows to the non-nullable form.
//! cites: 12.7, 7.4, 2.6, 2.18
//! expect: run
//! issue: 7342
//! stdout: 00000000-0000-0000-0000-000000000000

module Conf;

public class Program
  public static method Main()
    var x: Guid? := Guid.Empty;
    if x is not null then
      Console.WriteLine(x.ToString());
    end;
  end Main;
end Program;
