//! case: s26_5_0050
//! doc: spec
//! section: 26.5
//! title: an extension property named Result applies to a bare Task, which declares no Result
//! rule: If the receiver's type declares no instance member of that name, the compiler searches the
//!   current module and all imported modules
//! cites: 30.3, 26.3
//! expect: run
//! output: 5
module Conf;

extension Task
  public property Result: int => 5;
end extension;

public class Program
  public static async method V(): Task
    await Task.Delay(1);
  end V;

  public static method Main(): int
    using var b := V();
    b.Wait();
    Console.WriteLine(b.Result.ToString());
    return 0;
  end Main;
end Program;
