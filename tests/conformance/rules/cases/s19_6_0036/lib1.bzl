module Lib1;

public class L1
  public static method Next(d: DayOfWeek): DayOfWeek
    return (case d of
      DayOfWeek.Sunday => DayOfWeek.Monday,
      DayOfWeek.Monday => DayOfWeek.Tuesday,
      DayOfWeek.Tuesday => DayOfWeek.Wednesday,
      DayOfWeek.Wednesday => DayOfWeek.Thursday,
      DayOfWeek.Thursday => DayOfWeek.Friday,
      DayOfWeek.Friday => DayOfWeek.Saturday,
      DayOfWeek.Saturday => DayOfWeek.Sunday);
  end Next;
end L1;
