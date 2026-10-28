module Conf;
// §22.12 rule 4 statics obligation: its own declaration-site warning in both builds; not a trust line. EXPECT: the §22.12 warning only.
public class Widget
  public constructor()
  end constructor;
end Widget;
public class Registry
  public static readonly _shared: Widget := new Widget();
end Registry;
