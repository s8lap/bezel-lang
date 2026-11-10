module Model.Weekdays;

public enum Weekday
  Monday, Tuesday, Wednesday, Thursday, Friday, Saturday, Sunday;

  public method IsWeekend(): bool => this = Saturday or this = Sunday;
  public method Next(): Weekday => Weekday((int(this) + 1) mod 7);
end Weekday;

public class Program
  private static method ParseDay(s: string): Weekday?
    for var name in typeof(Weekday).EnumNames do
      if name.Equals(s, ignoreCase := true) then
        return Weekday.FromNameOrNull(name);
      end;
    end;
    return null;
  end ParseDay;

  public static method Main()
    var inputs: Array<string> := ["Monday", "saturday", "Funday"];
    for var s in inputs do
      var d := ParseDay(s);
      if d = null then
        Console.WriteLine($"{s}: not a weekday");
      else
        Console.WriteLine($"{Weekday.NameOf(d)} weekend={d.IsWeekend()} next={Weekday.NameOf(d.Next())}");
      end;
    end;
    Console.WriteLine($"Sunday next={Weekday.NameOf(Weekday.Sunday.Next())}");
    var weekend := 0;
    for var v in typeof(Weekday).EnumValues do
      if Weekday(v).IsWeekend() then weekend++; end;
    end;
    Console.WriteLine($"weekend days: {weekend}");
  end Main;
end Program;
