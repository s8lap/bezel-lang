module Concurrency.Workers;

public class Program
  private static method SumSquares(range: Array<int>): long
    var total := 0L;
    for var n in range[0]..range[1] do
      total += long(n) * long(n);
    end;
    return total;
  end SumSquares;

  public static async method Main(): Task
    const workers := 4;
    var tasks := new Array<Task<long>>();
    for var w in 0..<workers do
      var lo := w * 250 + 1;
      var hi := (w + 1) * 250;
      tasks.Add(Domain.Spawn(SumSquares, [lo, hi]));
    end;
    var results := await Task.WhenAll(tasks);
    Console.WriteLine($"sum of squares {results.Sum()}");
    Console.WriteLine($"workers {workers}");
  end Main;
end Program;
