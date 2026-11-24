// bug: NB12
// title: four or more tasks pending in `Semaphore.AcquireAsync` stall the program
// issue: new (possibly the NB11 runtime root)
// rests-on: stdlib §6.3: `AcquireAsync` awaits a permit; disposing the token returns it; entry order among waiters is unspecified
// source: r42/data/new-compiler-bugs.md, verbatim below this header
// note: Entry order is unspecified, so lines compare as a multiset.
// expect: run
// order: any
// stdout: started
// stdout: job 1 in
// stdout: job 1 out
// stdout: job 2 in
// stdout: job 2 out
// stdout: job 3 in
// stdout: job 3 out
// stdout: job 4 in
// stdout: job 4 out
// stdout: job 5 in
// stdout: job 5 out
// stdout: one awaited
// stdout: one awaited
// stdout: one awaited
// stdout: one awaited
// stdout: one awaited
module P;
import System.Threading;
public static class Jobs
  public static async method Run(id: int, gate: Semaphore): Task
    using var permit := await gate.AcquireAsync();
    Console.WriteLine($"job {id} in");
    await Task.Delay(10);
    Console.WriteLine($"job {id} out");
  end Run;
end Jobs;
public class Program
  public static async method Main(): Task
    using var gate := new Semaphore(1);
    using var jobs := new OwningArray<Task>();
    for var i in 1..5 do
      jobs.Add(Jobs.Run(i, gate));
    end;
    Console.WriteLine("started");
    for var j in jobs do
      await j;
      Console.WriteLine("one awaited");
    end;
  end Main;
end Program;
