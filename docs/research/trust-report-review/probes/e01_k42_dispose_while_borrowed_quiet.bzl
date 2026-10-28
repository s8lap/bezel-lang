module Conf;
// K4.2 site-announced kill (conditional, so not 0205's error). EXPECT: quiet under the mode.
public class Widget
  public constructor()
  end constructor;
  public method N(): int => 1;
end Widget;
public class Program
  public static method Run(flag: bool): int
    var a := new Widget();
    var b := a;
    if flag then
      a.Dispose();
    end;
    return b.N();
  end Run;
end Program;
