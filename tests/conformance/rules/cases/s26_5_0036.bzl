//! case: s26_5_0036
//! doc: spec
//! section: 26.5
//! title: an extension on string called in a loop on fresh concatenations returns the right lengths
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the current module and all imported modules ... Extensions are resolved on the compile-time type of the receiver, not the runtime type.
//! cites: 26.1
//! expect: run
//! output: 2400
module Conf;
extension string public method Repeat(n: int): string => this + this; end;
public class Program
  public static method Main(): int
    var i := 0;
    var total := 0;
    while i < 200 do
      var s := "ab" + "cd";
      var t := s.Repeat(2);
      total := total + t.Length + "xy".Repeat(1).Length;
      i := i + 1;
    end;
    Console.WriteLine(total);
    return 0;
  end Main;
end Program;
