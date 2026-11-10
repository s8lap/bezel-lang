module Format.Dates;

public class Program
  // r40: DayOfWeek.NameOf is not implemented (compiler bug B12); spell the names by hand.
  private static method Name(d: DayOfWeek): string => (case d of
    DayOfWeek.Sunday => "Sunday", DayOfWeek.Monday => "Monday", DayOfWeek.Tuesday => "Tuesday",
    DayOfWeek.Wednesday => "Wednesday", DayOfWeek.Thursday => "Thursday", DayOfWeek.Friday => "Friday",
    DayOfWeek.Saturday => "Saturday");

  public static method Main()
    // DateOnly has no AddDays / DayOfWeek / day arithmetic (stdlib §2.5); use DateTime throughout.
    var a := DateTime.Parse("2026-02-14");
    var b := DateTime.Parse("2026-11-10");
    Console.WriteLine($"{(b - a).Days} days");
    Console.WriteLine(a.AddDays(30).ToString("yyyy-MM-dd"));
    Console.WriteLine(Name(a.DayOfWeek));
    Console.WriteLine(Name(b.DayOfWeek));
    var span := TimeSpan.FromSeconds(90061);
    Console.WriteLine($"{span.Days}d {span.Hours:D2}:{span.Minutes:D2}:{span.Seconds:D2}");
  end Main;
end Program;
