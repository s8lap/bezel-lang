//! case: s8_6_4708
//! doc: spec
//! section: 8.6
//! title: a boxed Guid passes the Guid type test and a boxed null Guid? does not
//! rule: Unboxing reverses the process: a null `Object` reference unboxes to `null` of the target
//!   nullable type; a non-null boxed `T` unboxes to a non-null `T?`.
//! cites: 12.7, 5.1
//! expect: run
//! output: isGuid
//!   notGuid
module Conf;

public class Program
  public static method Main()
    var n: Guid? := Guid.Empty;
    var o: Object := n;
    if o is Guid then
      Console.WriteLine("isGuid");
    else
      Console.WriteLine("notGuid");
    end if;
    var m: Guid? := null;
    var oo: Object? := m;
    if oo is Guid then
      Console.WriteLine("isGuid");
    else
      Console.WriteLine("notGuid");
    end if;
  end Main;
end Program;
