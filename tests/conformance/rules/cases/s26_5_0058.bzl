//! case: s26_5_0058
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on the Task of a TaskCompletionSource is rejected
//! rule: Where neither rule 1 nor this search supplies the name, nothing
//!   supplies it: the access names no member and is a compile error at the
//!   name, the diagnostic naming the receiver's static type (§26.5 rule 2,
//!   v0.355).
//! cites: 26.5, 30.3, 30.11
//! expect: reject
//! diagnostic: Bogus
module Conf;

import System.Threading;

public class Program
  public static method Main(): int
    using var s := new TaskCompletionSource<int>();
    s.Task.Bogus();
    return 0;
  end Main;
end Program;
