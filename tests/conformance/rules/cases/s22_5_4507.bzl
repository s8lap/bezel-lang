//! case: s22_5_4507
//! doc: spec
//! section: 22.5
//! title: naming a parameter already bound positionally is a compile error
//! rule: naming a parameter already bound positionally (`Log("Failed", message :=
//!   "Retry")` against `Log(message: string, level: string := "INFO")`), or
//!   naming the same parameter twice, is a compile error at the call naming that
//!   parameter
//! cites: 22.6
//! expect: reject
//! diagnostic: bound
//! issue: 7602
module Conf;

class C
  public static method Main(): int
    Console.WriteLine(int.Parse("ff", s := "1"));
    return 0;
  end Main;
end C;
