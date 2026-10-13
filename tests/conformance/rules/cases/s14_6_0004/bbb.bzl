module Conf.Lhs;
import Conf.Visit;

public class LeftVisitor: IVisit
  public constructor()
  end constructor;

  public method M<T>(v: IVisit): int => v.N<T>(v);

  public method N<T>(v: IVisit): int => 1;
end LeftVisitor;
