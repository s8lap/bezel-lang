//! case: s28_6_0074
//! doc: spec
//! section: 28.6
//! title: the null-forgiving explicit disposal of a nullable value task is a
//!   disposal
//! rule: the `x?.Dispose()` / `x!.Dispose()` spellings
//! cites: 28.6, 28.3.1
//! expect: run
//! stdout: 7
module Conf;

public class Program
  public static async method W(): Task
    await Task.Delay(1);
  end W;
  public static async method WI(): Task<int>
    await Task.Delay(1);
    return 7;
  end WI;
  public static async method Main(): Task
    var t: Task<int>? := Program.WI();
    const v := await t!;
    t!.Dispose();
    Console.WriteLine(v);
  end Main;
end Program;
