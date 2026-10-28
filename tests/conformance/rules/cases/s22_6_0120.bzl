//! case: s22_6_0120
//! doc: spec
//! section: 22.6
//! title: int.TryParse of an int argument reaches no candidate
//! rule: Language spec §22.6 (v0.345): "If no candidate is applicable at all — no candidate is
//!   reachable: a conversion §5.1 does not admit as implicit … the call is a compile error with
//!   no candidates cited, because no overload was reachable."
//! cites: 22.6, 5.1, 9.1
//! expect: reject
//! diagnostic: no matching overload
module Conf;

public class Program
  public static method Main()
    var v := 0;
    var ok := int.TryParse(1, out v);
    Console.WriteLine(ok);
  end Main;
end Program;
