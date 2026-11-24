module Tools.Stats;

public class Program
  public static method Main(args: Array<string>): int
    var places := 2;
    var i := 0;
    while i < args.Count do
      if args[i] = "-p" and i + 1 < args.Count then
        var p := int.ParseOrNull(args[i + 1]);
        if p = null or p < 0 then
          Console.Error.WriteLine($"stats: bad precision '{args[i + 1]}'");
          return 2;
        end;
        places := p;
        i += 2;
      else
        Console.Error.WriteLine($"stats: unknown argument '{args[i]}'");
        return 2;
      end;
    end;

    var xs := new Array<double>();
    var n := 0;
    var line := Console.ReadLine();
    while line <> null do
      n++;
      var t := line.Trim();
      if t.Length > 0 then
        var v := double.ParseOrNull(t);
        if v = null then
          Console.Error.WriteLine($"stats: line {n}: not a number: {t}");
        else
          xs.Add(v);
        end;
      end;
      line := Console.ReadLine();
    end;
    if xs.Count = 0 then
      Console.Error.WriteLine("stats: no numbers");
      return 1;
    end;
    xs.Sort();
    var mid := xs.Count / 2;
    var median := (if xs.Count mod 2 = 0 then (xs[mid - 1] + xs[mid]) / 2.0 else xs[mid]);
    var f := $"F{places}";
    Console.WriteLine($"n={xs.Count} min={xs.Min().ToString(f)} max={xs.Max().ToString(f)} mean={xs.Average().ToString(f)} median={median.ToString(f)}");
    return 0;
  end Main;
end Program;
