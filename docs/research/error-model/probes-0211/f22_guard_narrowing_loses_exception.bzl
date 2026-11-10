// Probe f22 (proposal 0211 §5.2): the guard form retypes the binding. After
// `when ex is IRetryable` the arm sees `ex` at the interface type, and `Message`
// is not a member of it. Expected: rejected (spec §12.7, §7.4).
module F22;

public interface IRetryable
  method ShouldRetry(): bool;
end IRetryable;

public class Busy: Exception, IRetryable
  public constructor(msg: string) base(msg);
  end constructor;
  public method ShouldRetry(): bool => true;
end Busy;

public class Program
  public static method Main()
    try
      throw new Busy("server busy");
    catch
      var ex: Exception when ex is IRetryable => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
