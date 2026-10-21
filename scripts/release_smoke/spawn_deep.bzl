module SmokeSpawn;

// A Domain.Spawn worker recursing 20000 deep: it needs the runtime's explicit
// 8 MiB thread stack (#6695); musl's default thread stack is 128 KiB.
public class Program
  private static method Work(n: int): int
    var s := 0;
    for var i in 0..n do
      s := s + i;
    end;
    return s;
  end Work;

  private static method Deep(n: int): int
    if n = 0 then
      return 0;
    end;
    return 1 + Deep(n - 1);
  end Deep;

  public static async method Main(): Task
    using var t1 := Domain.Spawn(Work, 1000);
    using var t2 := Domain.Spawn(Deep, 20000);
    var r1 := await t1;
    var r2 := await t2;
    Console.WriteLine($"{r1} {r2}");
  end Main;
end Program;
