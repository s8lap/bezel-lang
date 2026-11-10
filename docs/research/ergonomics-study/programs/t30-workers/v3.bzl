module Concurrency.Workers;

record Slice(Lo: int, Hi: int);

public class Program
  private static method SumSquares(s: Slice): long
    var total := 0L;
    for var n in s.Lo..s.Hi do
      total += long(n) * long(n);
    end;
    return total;
  end SumSquares;

  public static async method Main(): Task
    const workers := 4;
    // v3: a loop over N workers, holding the tasks in an owning collection.
    using var tasks := new OwningArray<Task<long>>();
    for var w in 0..<workers do
      tasks.Add(Domain.Spawn(SumSquares, new Slice(w * 250 + 1, (w + 1) * 250)));
    end;
    var total := 0L;
    for var t in tasks do
      total += await t;
    end;
    Console.WriteLine($"sum of squares {total}");
    Console.WriteLine($"workers {workers}");
  end Main;
end Program;
