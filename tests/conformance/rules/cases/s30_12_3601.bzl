//! case: s30_12_3601
//! doc: spec
//! section: 30.12
//! title: Mutex.Acquire() inside a non-async lambda nested in an async method is not rejected at compile time
//! rule: the compiler rejects the blocking forms where the innermost enclosing method or lambda body is
//!   `async` (… a non-`async` lambda nested in an `async` body is not such a body, §30.3)
//! cites: 30.12, 30.3
//! expect: accept
//! issue: 8076
//! notes: The issue's shape. Contrast s30_12_0001, where the acquisition stands directly in the async
//!   method.
module Conf;

import System.Threading;

public class Program
  public static async method RunAsync(m: Mutex): Task
    var f := lambda() => do
      using var g := m.Acquire();
    end;
    f();
    await Task.Delay(0);
  end RunAsync;
end Program;
