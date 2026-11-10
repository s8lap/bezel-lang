module Concurrency.LockCounter;

import System.Threading;

public class Counter
  public property Value: int;
end Counter;

public class Program
  private static method Work(owned shared: Shared<Locked<Counter>>)
    for var i in 1..1000 do
      using var g := shared.Get().Lock();
      g.Value.Value := g.Value.Value + 1;
    end;
  end Work;

  private static method WorkAtomic(owned shared: Shared<AtomicInteger<int>>)
    for var i in 1..1000 do
      _ := shared.Get().Increment();
    end;
  end WorkAtomic;

  public static async method Main(): Task
    using var counter := new Shared<Locked<Counter>>(new Locked<Counter>(new Counter()));
    using var t1 := Domain.Spawn(Work, counter.Clone());
    using var t2 := Domain.Spawn(Work, counter.Clone());
    using var t3 := Domain.Spawn(Work, counter.Clone());
    using var t4 := Domain.Spawn(Work, counter.Clone());
    await t1; await t2; await t3; await t4;
    using var g := counter.Get().Lock();
    Console.WriteLine($"count {g.Value.Value}");

    using var atom := new Shared<AtomicInteger<int>>(new AtomicInteger<int>(0));
    using var a1 := Domain.Spawn(WorkAtomic, atom.Clone());
    using var a2 := Domain.Spawn(WorkAtomic, atom.Clone());
    using var a3 := Domain.Spawn(WorkAtomic, atom.Clone());
    using var a4 := Domain.Spawn(WorkAtomic, atom.Clone());
    await a1; await a2; await a3; await a4;
    Console.WriteLine($"atomic {atom.Get().Value}");
  end Main;
end Program;
