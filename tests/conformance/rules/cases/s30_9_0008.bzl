//! case: s30_9_0008
//! doc: spec
//! section: 30.9
//! title: a borrowed Pattern B operand whose fault escapes is retained by borrow, not disposed by the award
//! rule: A **borrowed** operand — a plain task parameter, a task read from a
//!   field or static storage, an owning-collection slot, a borrowing-iteration
//!   variable; a Pattern B operand among them, whose award spends it and
//!   disposes nothing (§30.5) — is retained by **borrow**
//! cites: 30.9, 30.5
//! expect: run
//! output: boom
//!   boom
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

  public async method Run(): Task
    using var r := await _t;
  end Run;
end Holder;

public class Demo
  public static async method Bad(): OwningTask<Res>
    await Task.Delay(10);
    throw new InvalidOperationException("boom");
  end Bad;

  public static async method Main(): Task
    using var h := new Holder(Bad());
    using var o := h.Run();
    for var i in 1..2 do
      try
        await o;
      catch
        var e: InvalidOperationException => Console.WriteLine(e.Message)
      end try;
    end;
  end Main;
end Demo;
