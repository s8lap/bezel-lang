module SmokeAsync;

// Task.Delay runs on the runtime's timer thread; the continuations on the pool.
public class Program
  private static async method Step(n: int): Task<int>
    await Task.Delay(5);
    return n * 2;
  end Step;

  public static async method Main(): Task
    var a := await Step(1);
    var b := await Step(a);
    Console.WriteLine($"async {a} {b}");
  end Main;
end Program;
