// Probe f21 (proposal 0211 §5.2): reaching an interface's own members when the
// binding is an `Exception`. Inside the arm, `ex as IRetryable` gives a nullable
// borrow at the interface type; the binding keeps the `Exception` surface.
//   (1) a class arm, with the interface reached through `as`;
//   (2) the guard form `when ex is IRetryable`: the arm sees `ex` at the
//       interface type (spec §12.7 retypes the carrier), so the interface member
//       is reachable and the `Exception` surface is not (f22).
module F21;

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
      var ex: Exception => do
        var r := ex as IRetryable;
        if r <> null then
          Console.WriteLine($"(1) retry = {r.ShouldRetry()}, message = {ex.Message}");
        end if;
      end,
    end try;

    try
      throw new Busy("server busy");
    catch
      var ex: Exception when ex is IRetryable => Console.WriteLine($"(2) retry = {ex.ShouldRetry()}"),
    end try;
  end Main;
end Program;
