// Probe f08 (proposal 0211 §5): a generic catch-arm parameter bounded by the
// marker interface alone. Spec §31.2.1: "The parameter must carry an
// `Exception`-derived class bound" — expected: rejected at the arm.
module F08;

public interface IFailure
end IFailure;

public class Check
  public static method Fails<T>(body: () -> ()): bool
      where T: IFailure
    try
      body();
    catch
      T => return true;
    end try;
    return false;
  end Fails;
end Check;

public class Program
  public static method Main()
  end Main;
end Program;
