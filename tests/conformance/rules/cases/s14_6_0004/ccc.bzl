module Conf.Rhs;
import Conf.Visit;

public class RightVisitor: IVisit
  public constructor()
  end constructor;

  public method M<T>(v: IVisit): int => 5;

  public method N<T>(v: IVisit): int => v.M<T>(v);
end RightVisitor;
