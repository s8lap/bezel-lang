// Probe f16 (proposal 0211 §5.4): a marker-typed value is not a throw operand.
// Spec §31.1: "An `Object`- or interface-typed operand is likewise rejected".
// A marker arm therefore re-raises with the bare `throw` (f05 case 6), never
// with `throw f`. Expected: rejected.
module F16;

public interface IFailure
end IFailure;

public class BadInput: Exception, IFailure
  public constructor(msg: string) base(msg);
  end constructor;
end BadInput;

public class Program
  public static method Main()
    try
      throw new BadInput("bad input");
    catch
      var f: IFailure => throw f,
    end try;
  end Main;
end Program;
