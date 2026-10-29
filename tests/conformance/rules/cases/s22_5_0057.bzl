//! case: s22_5_0057
//! doc: spec
//! section: 22.5
//! title: naming PadLeft's totalWidth twice by name is rejected
//! rule: naming a parameter already bound positionally (`Log("Failed", message := "Retry")` against `Log(message: string, level: string := "INFO")`), or naming the same parameter twice, is a compile error at the call naming that parameter
//! cites: 22.5
//! expect: reject
//! diagnostic: bound more than once
module Conf;

public class Program
  public static method Pad(s: string): string
    return s.PadLeft(totalWidth := 20, totalWidth := 21);
  end Pad;
end Program;
