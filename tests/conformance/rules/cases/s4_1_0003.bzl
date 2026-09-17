//! case: s4_1_0003
//! doc: spec
//! section: 4.1
//! title: an if condition of type bool compiles
//! rule: Conditions of if/elif and while require bool.
//! cites: 4.1
//! expect: accept
module Conf;

public class Checker
  public static method Run(n: int)
    if n <> 0 then
    end if;
  end Run;
end Checker;
