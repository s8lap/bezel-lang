module Text.Logfmt;

public static class Logfmt
  public static method Parse(line: string): owned Dictionary<string, string>
    var result := new Dictionary<string, string>();
    var i := 0;
    while i < line.Length do
      while i < line.Length and line[i] = ' ' do i++; end;
      if i >= line.Length then break; end;
      var eq := line.IndexOf('=', i);
      if eq < 0 then break; end;
      var key := line.Substring(i, eq - i);
      i := eq + 1;
      var sb := new StringBuilder();
      if i < line.Length and line[i] = '"' then
        i++;
        while i < line.Length and line[i] <> '"' do
          if line[i] = '\\' and i + 1 < line.Length then i++; end;
          sb.Append(line[i]);
          i++;
        end;
        i++;
      else
        while i < line.Length and line[i] <> ' ' do
          sb.Append(line[i]);
          i++;
        end;
      end;
      result[key] := sb.ToString();
    end;
    return result;
  end Parse;

  public static method Millis(dur: string): int => int.Parse(dur.Replace("ms", ""));
end Logfmt;

public class Program
  public static method Main()
    var lines: Array<string> := [
      "level=info msg=\"user login\" user=ann dur=12ms",
      "level=error msg=\"db \\\"main\\\" down\" dur=1500ms",
      "level=info msg=ok dur=3ms"
    ];
    var levels := new Dictionary<string, int>();
    var keys := new Set<string>();
    var slowest := "";
    var slowestMs := -1;
    for var line in lines do
      using var kv := Logfmt.Parse(line);
      var level := kv["level"];
      levels[level] := (levels.GetOrNull(level) ?? 0) + 1;
      for var e in kv do _ := keys.TryAdd(e.Key); end;
      var ms := Logfmt.Millis(kv["dur"]);
      if ms > slowestMs then
        slowestMs := ms;
        slowest := kv["msg"];
      end;
    end;
    for var e in levels do Console.WriteLine($"{e.Key} {e.Value}"); end;
    Console.WriteLine($"slowest: {slowest} ({slowestMs} ms)");
    var sorted := keys.ToArray();
    sorted.Sort();
    Console.WriteLine($"keys: {String.Join(", ", sorted)}");
  end Main;
end Program;
