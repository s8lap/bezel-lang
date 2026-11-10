module Model.Weekdays;

public enum Weekday
  Monday, Tuesday, Wednesday, Thursday, Friday, Saturday, Sunday;

  public method IsWeekend(): bool => this = Saturday or this = Sunday;
  public method Next(): Weekday => Weekday((int(this) + 1) mod 7);
end Weekday;

public class Program
  private static method ParseDay(s: string): Weekday?
    for var i in 0..6 do
      var d := Weekday(i);
      if Name(d).CompareTo(s, ignoreCase := true) = 0 then
        return d;
      end;
    end;
    return null;
  end ParseDay;

  private static method Name(d: Weekday): string
    return (case d of
      Weekday.Monday => "Monday", Weekday.Tuesday => "Tuesday", Weekday.Wednesday => "Wednesday",
      Weekday.Thursday => "Thursday", Weekday.Friday => "Friday", Weekday.Saturday => "Saturday",
      Weekday.Sunday => "Sunday");
  end Name;

  public static method Main()
    var inputs: Array<string> := ["Monday", "saturday", "Funday"];
    for var s in inputs do
      var d := ParseDay(s);
      if d = null then
        Console.WriteLine($"{s}: not a weekday");
      else
        Console.WriteLine($"{Name(d)} weekend={d.IsWeekend()} next={Name(d.Next())}");
      end;
    end;
    Console.WriteLine($"Sunday next={Name(Weekday.Sunday.Next())}");
    var weekend := 0;
    for var i in 0..6 do
      if Weekday(i).IsWeekend() then weekend++; end;
    end;
    Console.WriteLine($"weekend days: {weekend}");
  end Main;
end Program;
