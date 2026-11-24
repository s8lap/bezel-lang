module Lib;
public class Fl
  public property [q: () -> int]: int => q();
  public constructor() end constructor;
end Fl;
public class Holder
  public F: () -> int := lambda() => 1;
  public constructor() end constructor;
end Holder;
