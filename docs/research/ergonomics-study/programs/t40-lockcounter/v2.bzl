module Concurrency.LockCounter;

import System.Threading;

// Locked<T> is not implemented (#1721), and Shared<T> is sendable only at the stdlib's
// synchronized types, so a user counter guarded by a Mutex cannot be shared across domains.
// Only the atomic half of the task is expressible.
public class Program
  private static method WorkAtomic(owned shared: Shared<AtomicInteger<int>>)
    using var s := move shared;
    for var i in 1..1000 do
      _ := s.Get().Increment();
    end;
  end WorkAtomic;

  public static async method Main(): Task
    using var atom := new Shared<AtomicInteger<int>>(new AtomicInteger<int>(0));
    using var a1 := Domain.Spawn(WorkAtomic, atom.Clone());
    using var a2 := Domain.Spawn(WorkAtomic, atom.Clone());
    using var a3 := Domain.Spawn(WorkAtomic, atom.Clone());
    using var a4 := Domain.Spawn(WorkAtomic, atom.Clone());
    await a1;
    await a2;
    await a3;
    await a4;
    Console.WriteLine($"atomic {atom.Get().Value}");
  end Main;
end Program;
