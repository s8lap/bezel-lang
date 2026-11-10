// Probe p18: what a binding of marker-interface type can read. Both arm forms
// compile; inside either, the binding exposes only the interface's members
// (the `is` guard narrows `ex` to the interface, spec §12.7), so ToString() is
// reachable and Message is not. Two variants were rejected on r40 and are
// recorded here as comments:
//   var ex: Exception when ex is IFailure => ... ex.Message ...
//     -> "no member `Message` on interface `IFailure`"
//   public interface IFailure  readonly property Message: string;  end IFailure;
//     -> "class `BadInput` does not implement interface property `Message` —
//         a field does not satisfy a property contract" (Message is inherited
//         from Exception)
module P18;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Program
  public static method Main()
    try
      throw new BadInput("not a number");
    catch
      var f: IFailure => Console.WriteLine($"interface binding: {f.ToString()}"),
    end try;
    try
      throw new BadInput("again");
    catch
      var ex: Exception when ex is IFailure => Console.WriteLine($"guard form: {ex.ToString()}"),
    end try;
  end Main;
end Program;
