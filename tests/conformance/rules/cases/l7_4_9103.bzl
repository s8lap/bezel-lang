//! case: l7_4_9103
//! doc: stdlib
//! section: 7.4
//! title: a `var` local from Console.In.ReadToEnd() can be stored into string collections
//! rule: public method ReadToEnd(): string;
//! cites: 7.4, 9.6
//! expect: run
//! output: n=2
//!   l=0
//!   []
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
    var lst := new LinkedList<string>();
    lst.AddLast(raw.Trim());
    lst.AddLast(raw);
    Console.WriteLine("n=" + lst.Count.ToString());
    var arr := new Array<string>();
    arr.Add(raw);
    arr.Add(raw.ToUpper());
    Console.WriteLine("l=" + arr[0].Length.ToString());
    Console.WriteLine("[" + arr[1] + "]");
  end Main;
end Program;
