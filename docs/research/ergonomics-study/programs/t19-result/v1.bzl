module Errors.ResultPipeline;

public static class Steps
  public static method ParseInt(s: string): Result<int, string>
    var n := int.ParseOrNull(s);
    if n = null then
      return Error($"not a number: {s}");
    end;
    return Ok(n);
  end ParseInt;

  public static method CheckEven(n: int): Result<int, string>
    return (if n mod 2 = 0 then Ok(n) else Error($"{n} is odd"));
  end CheckEven;

  public static method Half(n: int): Result<int, string> => Ok(n / 2);
end Steps;

public class Program
  public static method Main()
    var inputs: Array<string> := ["8", "7", "x"];
    for var s in inputs do
      var r := Steps.ParseInt(s).AndThen(Steps.CheckEven).AndThen(Steps.Half);
      case r of
        Ok(var v) => Console.WriteLine($"{s} -> {v}"),
        Error(var m) => Console.WriteLine($"{s} -> error: {m}")
      end;
    end;
  end Main;
end Program;
