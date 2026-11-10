// Probe f11 (proposal 0211 §6.2): the band rides the task fault unchanged.
//   (1) `await` in a `try` whose arm is the marker: a failure is taken;
//   (2) the same with a bug: it passes the marker arm;
//   (3) two awaiters of one faulted task both see the marker;
//   (4) `Task.WhenAll` over a failure and a bug: the first non-successful input in
//       input order is re-raised (spec §30.10), whichever completed first.
module F11;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Res
  private _name: string;
  public constructor(name: string)
    _name := name;
    Console.WriteLine($"  open {name}");
  end constructor;
  destructor
    Console.WriteLine($"  close {_name}");
  end destructor;
end Res;

public class Program
  public static async method Work(kind: int, delay: int): Task<int>
    using var conn := new Res($"conn{kind}");
    await Task.Delay(delay);
    if kind = 1 then
      throw new BadInput("bad reply");
    elif kind = 2 then
      using var xs := new Array<int>(1, 0);
      return xs[3];
    end if;
    return 7;
  end Work;

  public static async method Main(): Task
    Console.WriteLine("(1) failure through await:");
    try
      var n := await Program.Work(1, 1);
      Console.WriteLine(n);
    catch
      IFailure => Console.WriteLine("  marker arm took it"),
    end try;

    Console.WriteLine("(2) bug through await:");
    try
      try
        var n := await Program.Work(2, 1);
        Console.WriteLine(n);
      catch
        IFailure => Console.WriteLine("  marker arm took it"),
      end try;
    catch
      var ex: Exception => Console.WriteLine($"  passed the marker arm: {ex.ToString()}"),
    end try;

    Console.WriteLine("(3) two awaiters:");
    using var t := Program.Work(1, 1);
    for var i in 1..2 do
      try
        var n := await t;
        Console.WriteLine(n);
      catch
        IFailure => Console.WriteLine($"  awaiter {i}: marker arm"),
      end try;
    end for;

    Console.WriteLine("(4) WhenAll, the bug completes first:");
    using var a := Program.Work(1, 20);
    using var b := Program.Work(2, 1);
    var both: Array<Task> := [a, b];
    try
      await Task.WhenAll(both);
    catch
      IFailure => Console.WriteLine("  WhenAll re-raised the failure"),
      var ex: Exception => Console.WriteLine($"  WhenAll re-raised the bug: {ex.ToString()}"),
    end try;
  end Main;
end Program;
