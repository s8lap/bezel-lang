//! case: s30_5_0020
//! doc: spec
//! section: 30.5
//! title: awarding an owned-field OwningTask through a borrow spends it, and a second award raises
//! rule: When the operand is **borrowed** — ... an `owned` field read without
//!   `move`, ... — the await **spends** the task (§30.4: the first award,
//!   whatever its outcome) and disposes **nothing** ... a field by the object's
//!   cascade
//! cites: 30.5, 30.4, 28.6
//! expect: run
//! output: awarded
//!   closed
//!   spent
//!   after
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;

public class Holder
  private owned _t: OwningTask<Res>;

  public constructor(owned t: OwningTask<Res>)
    _t := move t;
  end constructor;

  public async method Take(): Task
    using var r := await _t;
    Console.WriteLine("awarded");
  end Take;
end Holder;

public class Demo
  public static async method Make(): OwningTask<Res>
    await Task.Delay(1);
    return new Res();
  end Make;

  public static async method Main(): Task
    do
      using var h := new Holder(Make());
      await h.Take();
      try
        await h.Take();
      catch
        InvalidOperationException => Console.WriteLine("spent")
      end try;
    end;
    Console.WriteLine("after");
  end Main;
end Demo;
