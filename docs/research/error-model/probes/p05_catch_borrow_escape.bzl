// Probe p05 (expected: rejected). A caught exception is a borrow scoped to the
// arm; it cannot be stored for later (spec §31.1). This is why there is no
// first-class "caught error value" today.
module P05;

public class Program
  public static method Main()
    var kept: Exception? := null;
    try
      throw new FormatException("bad");
    catch
      var ex: FormatException => do
        kept := ex;
      end,
    end try;
    Console.WriteLine(kept = null);
  end Main;
end Program;
