//! case: s31_4_0007
//! doc: spec
//! section: 31.4
//! title: no Exception constructor accepts a SupersededChain argument
//! rule: No constructor populates or accepts `SupersededChain` — the chain is
//!   runtime-populated exactly as `StackTrace` is, so no constructor's
//!   purity classification changes.
//! cites: 31.4
//! expect: reject
//! diagnostic: constructor
module Conf;

public class Program
  public static method Main()
    using var cause := new Exception("cause");
    using var e := new Exception("msg", cause, cause.SupersededChain);
  end Main;
end Program;
