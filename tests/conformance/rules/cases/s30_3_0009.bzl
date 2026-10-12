//! case: s30_3_0009
//! doc: spec
//! section: 30.3
//! title: Wait() outside any domain blocks to settlement and returns
//! rule: `Wait()` and `Result` are synchronous escape hatches for **code
//!   running outside any domain** — in practice, the synchronous portion of
//!   program startup: a non-`async` `Main` ... There, they block the calling
//!   thread until the task completes.
//! cites: 30.3
//! expect: run
//! stdout: waited
module Conf;

public class Program
  public static method Main()
    using var t := Task.Delay(0);
    t.Wait();
    Console.WriteLine("waited");
  end Main;
end Program;
