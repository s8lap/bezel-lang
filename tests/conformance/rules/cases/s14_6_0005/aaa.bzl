module Conf.Visit;

public class Box<T>
  public constructor()
  end constructor;
end Box;

public interface IVisit
  method M<T>(v: IVisit): int;
  method N<T>(v: IVisit): int;
end IVisit;
