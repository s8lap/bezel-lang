//! case: s8_6_4701
//! doc: spec
//! section: 8.6
//! title: a narrowed Guid? boxed to Object is a boxed Guid: type test, rendering and equality
//! rule: If the value is non-null, the result is a boxed `T` (the non-nullable form). The nullable
//!   wrapper is stripped during boxing.
//! cites: 5.1, 7.4, 12.7
//! expect: run
//! output: True
//!   00000000-0000-0000-0000-000000000000
//!   True
module Conf;

public class Program
  public static method Main()
    var n: Guid? := Guid.Empty;
    var o: Object := n;
    Console.WriteLine(o is Guid);
    Console.WriteLine(o.ToString());
    Console.WriteLine(o.Equals(Guid.Empty));
  end Main;
end Program;
