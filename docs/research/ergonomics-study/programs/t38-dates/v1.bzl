module Format.Dates;

public class Program
  public static method Main()
    var a := DateOnly.Parse("2026-02-14");
    var b := DateOnly.Parse("2026-11-10");
    Console.WriteLine($"{b.DayNumber - a.DayNumber} days");
    Console.WriteLine(a.AddDays(30).ToString("yyyy-MM-dd"));
    Console.WriteLine(DayOfWeek.NameOf(a.DayOfWeek));
    Console.WriteLine(DayOfWeek.NameOf(b.DayOfWeek));
    var span := TimeSpan.FromSeconds(90061);
    Console.WriteLine($"{span.Days}d {span.Hours:D2}:{span.Minutes:D2}:{span.Seconds:D2}");
  end Main;
end Program;
