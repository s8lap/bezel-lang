//! case: s12_7_0044
//! doc: spec
//! section: 12.7
//! title: a narrowed nullable value type formats its payload, not the carrier
//! rule: `is null` tests whether the value is null. `is not null` tests whether it
//!   is non-null and narrows to the non-nullable form.
//! cites: 12.7, 7.4, 9.1, 2.18
//! expect: run
//! stdout: 00005

module Conf;

public class Program
  public static method Main()
    var x: int? := 5;
    if x is not null then
      Console.WriteLine(x.ToString("D5"));
    end;
  end Main;
end Program;
