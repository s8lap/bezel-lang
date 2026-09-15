module Mid;
import Base;

public class Wrapper
  public static method Make(cents: int): owned Money => new Money(cents);
end Wrapper;
