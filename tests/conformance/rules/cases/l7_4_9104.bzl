//! case: l7_4_9104
//! doc: stdlib
//! section: 7.4
//! title: a `string`-annotated local from Console.In.ReadToEnd() reads the same
//! rule: public method ReadToEnd(): string;
//! cites: 7.4, 9.6
//! expect: run
//! output: up=[]
//!   len=0
//! issue: 9057
//! notes: Pins #9057 (pick 317172e99): a `var` local initialised from `Console.In.ReadToEnd()` is
//!   typed `string`, so every 9.6 string member and `+` concatenation reads on it. The corpus
//!   runner supplies an empty stdin (/dev/null), so the local is the empty string (ReadToEnd
//!   reads the remaining text; none remains). Before the fix the local was not typed `string`:
//!   the `+` below was rejected and the member calls did not lower to the text. Spec-decided
//!   control: the declared-type form was always typed; green on both arms.
module Conf;

public class Program
  public static method Main()
    var s: string := Console.In.ReadToEnd();
    Console.WriteLine("up=[" + s.ToUpper() + "]");
    Console.WriteLine("len=" + s.Length.ToString());
  end Main;
end Program;
