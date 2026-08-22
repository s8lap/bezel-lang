public abstract class Shape
  public abstract method Area(): decimal;
  public abstract property Name: string;

  public method Describe(): string => $"A {Name} with area {Area()}";
end Shape;
