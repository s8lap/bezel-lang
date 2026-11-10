// Probe f05 (proposal 0211 §5): the catch-arm forms over a marker interface.
//   (1) a bare interface arm ahead of the catch-all;
//   (2) an interface listed in a multi-type alternative, no binding;
//   (3) an interface arm placed AFTER a class arm it overlaps (order decides);
//   (4) a class arm placed after an interface arm that covers it — spec §31.2.3
//       says this draws the unreachable-arm warning;
//   (5) a guarded interface arm;
//   (6) bare re-raise from a marker arm.
module F05;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Program
  static method Raise(which: int)
    if which = 0 then
      throw new BadInput("bad input");
    elif which = 1 then
      throw new InvalidOperationException("a bug");
    elif which = 2 then
      throw new OperationCanceledException("cancelled");
    end if;
  end Raise;

  public static method Main()
    for var i in 0..2 do
      try
        Program.Raise(i);
      catch
        IFailure => Console.WriteLine($"(1) case {i}: failure arm"),
        OperationCanceledException => Console.WriteLine($"(1) case {i}: cancellation arm"),
        Exception => Console.WriteLine($"(1) case {i}: catch-all arm"),
      end try;
    end for;

    try
      Program.Raise(0);
    catch
      TimeoutException | IFailure => Console.WriteLine("(2) alternative with an interface matched"),
    end try;

    try
      Program.Raise(0);
    catch
      BadInput => Console.WriteLine("(3) the class arm, written first, wins"),
      IFailure => Console.WriteLine("(3) not reached for BadInput"),
    end try;

    try
      Program.Raise(0);
    catch
      IFailure => Console.WriteLine("(4) the interface arm, written first, wins"),
      BadInput => Console.WriteLine("(4) unreachable"),
    end try;

    var retries := 0;
    try
      Program.Raise(0);
    catch
      IFailure when retries > 0 => Console.WriteLine("(5) not taken: guard false"),
      Exception => Console.WriteLine("(5) guard declined, next arm taken"),
    end try;

    try
      try
        Program.Raise(0);
      catch
        IFailure => throw,
      end try;
    catch
      var ex: BadInput => Console.WriteLine($"(6) re-raised with its type: {ex.Message}"),
    end try;
  end Main;
end Program;
