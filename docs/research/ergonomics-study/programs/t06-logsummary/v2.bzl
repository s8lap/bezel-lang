module Tools.LogSummary;

record Entry(At: DateTime, Level: string, Message: string);

public class Program
  public static method Main()
    const log := "2026-01-05T10:00:00 INFO start\n2026-01-05T10:02:30 ERROR disk full\n2026-01-05T10:03:00 WARN retrying\n2026-01-05T10:15:45 ERROR disk full again\n2026-01-05T10:20:00 INFO stop";
    var entries := new Array<Entry>();
    for var line in log.Split('\n') do
      var parts := line.Split(' ');
      var at := DateTime.Parse(parts[0]);
      var msg := String.Join(" ", parts.Skip(2));
      var level := parts[1];
      entries.Add(new Entry(at, level, msg));
    end;
    var counts := new Dictionary<string, int>();
    for var e in entries do
      counts[e.Level] := (counts.GetOrNull(e.Level) ?? 0) + 1;
    end;
    for var kv in counts do
      Console.WriteLine($"{kv.Key} {kv.Value}");
    end;
    var errors := entries.Where(lambda(e) => e.Level = "ERROR").ToArray();
    var first := errors.First().At;
    var last := errors.Last().At;
    var span := last - first;
    Console.WriteLine($"first={first:HH:mm:ss} last={last:HH:mm:ss} span={span}");
  end Main;
end Program;
