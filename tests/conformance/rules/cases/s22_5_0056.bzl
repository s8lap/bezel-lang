//! case: s22_5_0056
//! doc: spec
//! section: 22.5
//! title: naming PadLeft's totalWidth by name after binding it positionally is rejected
//! rule: naming a parameter already bound positionally (`Log("Failed", message := "Retry")` against `Log(message: string, level: string := "INFO")`), or naming the same parameter twice, is a compile error at the call naming that parameter
//! cites: 22.5
//! expect: reject
//! diagnostic: bound more than once
module Conf;

public class Program
  public static method Pad(s: string): string
    return s.PadLeft(20, totalWidth := 20);
  end Pad;
end Program;
