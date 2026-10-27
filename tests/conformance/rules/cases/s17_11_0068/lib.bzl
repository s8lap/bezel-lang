module L;

public record Pt(X: int, Y: int);
public enum Color Red, Green, Blue; end Color;
public interface IFoo end IFoo;
public class Sq: IFoo public constructor() end constructor; end Sq;

public static class U
  public static method DefB<T>(): string => $"{default(T)}";
  public static method Wrap<T>(): string => DefB<T>();
  public static method Plain<T>(): int => 5;
end U;

public class Holder
  public method DefB<T>(): string => $"{default(T)}";
  public method Wrap<T>(): string => this.DefB<T>();
end Holder;

public class Box<T>
  public static method DefB<T2>(): string => $"{default(T2)}";
  public static method Wrap<T2>(): string => DefB<T2>();
end Box;
