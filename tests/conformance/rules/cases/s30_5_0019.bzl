//! case: s30_5_0019
//! doc: spec
//! section: 30.5
//! title: awarding a using var OwningTask spends it without disposing it, and the scope end disposes it
//! rule: When the operand is **borrowed** — a `using var` binding, ... — the
//!   await **spends** the task (§30.4: the first award, whatever its outcome)
//!   and disposes **nothing**: the award is not a kill, no discharge is owed
//!   for the operand, and the task stays owned by its holder, which disposes it
//!   as it disposes any other task it holds — a `using var` at its scope end
//! cites: 30.5, 30.4
//! expect: run
//! output: spent
//!   last
//!   closed
//!   after
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;

public class Demo
  public static async method Make(): OwningTask<Res>
    await Task.Delay(1);
    return new Res();
  end Make;

  public static async method Main(): Task
    do
      using var t := Make();
      using var r := await t;
      try
        using var again := await t;
      catch
        InvalidOperationException => Console.WriteLine("spent")
      end try;
      Console.WriteLine("last");
    end;
    Console.WriteLine("after");
  end Main;
end Demo;
