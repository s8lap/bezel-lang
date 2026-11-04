//! case: s32_10_0007
//! doc: spec
//! section: 32.10
//! title: a blocking extern call inside a domain continuation is defined behavior, not an error
//! rule: **Blocking extern calls.** An extern call blocks its thread like any
//!   synchronous call: inside a domain continuation it stalls that domain (no other
//!   continuation of the domain runs) and pins one pool worker for the duration
//!   (§30.13). This is defined behavior, not an error
//! cites: 32.10, 30.13, 33.2
//! expect: run
//! output: before
//!   after
//! notes: contrast Mutex.Acquire, whose blocking form is a compile error in an async
//!   body; an extern call carries no such rule.
module Conf;

extern "libc"
  public method usleep(usec: uint): int;
end;

public class Program
  public static async method Main(): Task
    Console.WriteLine("before");
    await Task.Delay(1);
    _ := usleep(1000U);
    Console.WriteLine("after");
  end Main;
end Program;
