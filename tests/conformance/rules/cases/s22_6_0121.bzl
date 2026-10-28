//! case: s22_6_0121
//! doc: spec
//! section: 22.6
//! title: Guid.TryParse of an int argument reaches no candidate
//! rule: Language spec §22.6 (v0.345): "If no candidate is applicable at all — no candidate is
//!   reachable: a conversion §5.1 does not admit as implicit … the call is a compile error with
//!   no candidates cited, because no overload was reachable."
//! cites: 22.6, 5.1, 2.6
//! expect: reject
//! diagnostic: no matching overload
module Conf;

public class Program
  public static method Main()
    var v := Guid.Empty;
    var ok := Guid.TryParse(1, out v);
    Console.WriteLine(ok);
  end Main;
end Program;
