//! case: s8_6_0003
//! doc: spec
//! section: 8.6
//! title: boxing a nullable value type yields a null reference or a boxed non-nullable
//! rule: When a nullable value type `T?` is boxed: if the value is `null`, the
//!   result is a null `Object` reference; if the value is non-null, the result is
//!   a boxed `T` — the nullable wrapper is stripped during boxing.
//! cites: 7.2
//! expect: run
//! output: True
//!   False
module Conf;

public class Program
  public static method Main()
    var absent: int? := null;
    var present: int? := 7;
    var boxedAbsent: Object? := absent;
    var boxedPresent: Object? := present;
    Console.WriteLine(boxedAbsent = null);
    Console.WriteLine(boxedPresent = null);
  end Main;
end Program;
