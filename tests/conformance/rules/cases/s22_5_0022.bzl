//! case: s22_5_0022
//! doc: spec
//! section: 22.5
//! title: naming a parameter already bound positionally is a compile error at the call
//! rule: "naming a parameter already bound positionally (`Log("Failed", message := "Retry")` against
//!   `Log(message: string, level: string := "INFO")`), or naming the same parameter twice, is a compile
//!   error at the call naming that parameter"
//! cites: 22.5
//! expect: reject
//! diagnostic: text
//! notes: Pad's second parameter has a default, so the call supplies every required parameter and the
//!   only ground for rejection is the duplicate binding. The twin is s22_5_0023.
module Conf;

public class Program
  public static method Pad(text: string := "", width: int := 0): string
    return $"{text}{width}";
  end Pad;

  public static method Main()
    Console.WriteLine(Pad("a", text := "b"));
  end Main;
end Program;
