module LibA;
public class FlA
  public Tag: int;
  public virtual property [q: () -> int]: int
    get => q() + Tag;
    set => do Tag := q() + value; end;
  end;
  public constructor() Tag := 1; end constructor;
end FlA;
