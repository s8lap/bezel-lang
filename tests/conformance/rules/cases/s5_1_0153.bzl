//! case: s5_1_0153
//! doc: spec
//! section: 5.1
//! title: An int property reached through a chained call is not implicitly convertible to string
//! rule: The chained read d.AddDays(3).Year has type int, which is not implicitly convertible to string (spec 5.1).
//! cites: 5.1, 5.4
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var d := new DateTime(2024, 3, 15);
    var s: string := d.AddDays(3).Year;
    Console.WriteLine(s);
  end Main;
end Program;
