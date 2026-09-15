//! case: s5_1_0006
//! doc: spec
//! section: 5.1
//! title: float implicitly converts to double
//! rule: float implicitly converts to double (the §5.1 implicit-conversion table).
//! cites: 5.1
//! expect: accept
module Conf;

public class Program
  public static method Convert(f: float)
    var d: double := f;
  end Convert;
end Program;
