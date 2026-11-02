//! case: s26_5_0058
//! doc: spec
//! section: 26.5
//! title: an undeclared method called on the Task of a TaskCompletionSource is rejected
//! rule: If the receiver's type declares no instance member of that name, the
//!   compiler searches the current module and all imported modules
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
