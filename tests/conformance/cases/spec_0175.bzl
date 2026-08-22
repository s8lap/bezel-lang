import System.Collections.Linq;      // ToArray is a Linq extension (stdlib §5)

extension IEnumerable<T> where T: IComparable<T>
  public method Sort(): owned Array<T>
    var items := this.ToArray();
    // ...
    return items;
  end Sort;
end;
