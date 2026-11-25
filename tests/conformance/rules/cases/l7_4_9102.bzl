//! case: l7_4_9102
//! doc: stdlib
//! section: 7.4
//! title: a `var` local from Console.In.ReadToEnd() can be reassigned from a string member and read
//!   again
//! rule: public method ReadToEnd(): string;
//! cites: 7.4, 9.6
//! expect: run
//! output: a=[]
//!   b=0
//!   c=[]
//! issue: 9057
//! notes: Pins #9057 (pick 317172e99): a `var` local initialised from `Console.In.ReadToEnd()` is
//!   typed `string`, so every 9.6 string member and `+` concatenation reads on it. The corpus
//!   runner supplies an empty stdin (/dev/null), so the local is the empty string (ReadToEnd
//!   reads the remaining text; none remains). Before the fix the local was not typed `string`:
//!   the `+` below was rejected and the member calls did not lower to the text.
module Conf;

public class Program
  public static method Main()
    var raw := Console.In.ReadToEnd();
    raw := raw.ToUpper();
    Console.WriteLine("a=[" + raw + "]");
    var r2 := Console.In.ReadToEnd();
    Console.WriteLine("b=" + r2.Length.ToString());
    Console.WriteLine("c=[" + r2 + raw + "]");
  end Main;
end Program;
