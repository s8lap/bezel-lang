module Tools.Grep;

public class Program
  public static method Main(args: Array<string>): int
    var ignoreCase := false;
    var invert := false;
    var pattern: string? := null;
    for var a in args do
      case a of
        "-i" => ignoreCase := true,
        "-v" => invert := true,
        _ => pattern := a
      end;
    end;
    if pattern = null then
      Console.Error.WriteLine("usage: grep [-i] [-v] PATTERN");
      return 2;
    end;
    var needle := (if ignoreCase then pattern.ToLower() else pattern);
    var n := 0;
    var matched := false;
    var line := Console.ReadLine();
    while line <> null do
      n++;
      var hay := (if ignoreCase then line.ToLower() else line);
      if hay.Contains(needle) <> invert then
        Console.WriteLine($"{n}:{line}");
        matched := true;
      end;
      line := Console.ReadLine();
    end;
    return (if matched then 0 else 1);
  end Main;
end Program;
