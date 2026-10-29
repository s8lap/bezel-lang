//! case: s19_6_0034
//! doc: spec
//! section: 19.6
//! title: A case expression naming all seven DayOfWeek constants is statically exhaustive
//! rule: Enum scrutinee: "every declared constant has a matching pattern" proves exhaustiveness (§19.6, v0.350), so an expression `case` over the seven stdlib DayOfWeek constants (stdlib §2.5) with no `_` arm draws no warning.
//! cites: 19.6, 2.5
//! expect: quiet
module Conf;

public class Program
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

end Program;
