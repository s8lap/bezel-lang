//! case: l7_4_9101
//! doc: stdlib
//! section: 7.4
//! title: string members and concatenation work on a `var` local from Console.In.ReadToEnd()
//! rule: public method ReadToEnd(): string;
//! cites: 7.4, 9.6
//! expect: run
//! output: len=0
//!   trim=[]
//!   up=[]
//!   has=no
//!   idx=-1
//!   rep=[]
//!   <>
//!   sw=no
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
    Console.WriteLine("len=" + raw.Length.ToString());
    Console.WriteLine("trim=[" + raw.Trim() + "]");
    Console.WriteLine("up=[" + raw.ToUpper() + "]");
    Console.WriteLine("has=" + (if raw.Contains("l") then "yes" else "no"));
    Console.WriteLine("idx=" + raw.IndexOf("l").ToString());
    Console.WriteLine("rep=[" + raw.Replace("l", "L") + "]");
    Console.WriteLine($"<{raw}>");
    Console.WriteLine("sw=" + (if raw.StartsWith("H") then "yes" else "no"));
  end Main;
end Program;
