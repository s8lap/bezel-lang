module Tools.WordFreq;

public class Program
  public static method Main()
    var text := Console.In.ReadToEnd().ToLower();
    var counts := new Dictionary<string, int>();
    var sb := new StringBuilder();
    for var c in text do
      if Char.IsLetter(c) then
        sb.Append(c);
      elif sb.Length > 0 then
        var w := sb.ToString();
        counts[w] := (counts.GetOrNull(w) ?? 0) + 1;
        sb.Clear();
      end;
    end;
    if sb.Length > 0 then
      var w := sb.ToString();
      counts[w] := (counts.GetOrNull(w) ?? 0) + 1;
    end;
    var top := counts.OrderByDescending(lambda(e) => e.Value).ThenBy(lambda(e) => e.Key).Take(5);
    for var e in top do
      Console.WriteLine($"{e.Key} {e.Value}");
    end;
  end Main;
end Program;
