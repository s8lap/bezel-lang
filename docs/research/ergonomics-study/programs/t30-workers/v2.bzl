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
    // Fresh tasks cannot go into a borrowing Array (§15.4/§28.11); name each one.
    using var t0 := Domain.Spawn(SumSquares, new Slice(1, 250));
    using var t1 := Domain.Spawn(SumSquares, new Slice(251, 500));
    using var t2 := Domain.Spawn(SumSquares, new Slice(501, 750));
    using var t3 := Domain.Spawn(SumSquares, new Slice(751, 1000));
    var total := await t0 + await t1 + await t2 + await t3;
    Console.WriteLine($"sum of squares {total}");
    Console.WriteLine($"workers {workers}");
  end Main;
end Program;
