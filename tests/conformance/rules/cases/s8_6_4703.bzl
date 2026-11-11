//! case: s8_6_4703
//! doc: spec
//! section: 8.6
//! title: a Guid? boxed to Object? is null for a null value and a boxed Guid otherwise
//! rule: If the value is `null`, the result is a null `Object` reference.
//! cites: 5.1, 7.4
//! expect: run
//! output: True
//!   False
//!   00000000-0000-0000-0000-000000000000
module Conf;

public class Program
  public static method F(n: Guid?): Object?
    var o: Object? := n;
    return o;
  end F;

  public static method Main()
    var a := Program.F(Guid.Empty);
    var b := Program.F(null);
    Console.WriteLine(a <> null);
    Console.WriteLine(b <> null);
    if a <> null then
      Console.WriteLine(a!.ToString());
    end if;
  end Main;
end Program;
