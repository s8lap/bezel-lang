//! case: l7_4_9951
//! doc: stdlib
//! section: 7.4
//! title: Console.In.Read() and ReadLine() interleaved at end of input answer -1 and null each time
//! rule: null at EOF
//! cites: 7.4, 2.1
//! expect: run
//! output: -1 null -1 null -1
//!   e null
//! notes: Pins #9260. Empty stdin: every Read() is -1 and every ReadLine() is null, whatever the order.
//!   The interpolation renders a null string as null (spec 18.5). The second line is printed
//!   because e is null.
module Conf;
public class Program
  public static method Main(): int
    var a := Console.In.Read();
    var b := Console.In.ReadLine();
    var c := Console.In.Read();
    var d := Console.In.ReadLine();
    var e := Console.In.ReadLine();
    var f := Console.In.Read();
    Console.WriteLine($"{a} {b} {c} {d} {f}");
    if e = null then Console.WriteLine("e null"); end;
    return 0;
  end Main;
end Program;
