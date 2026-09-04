extension IEnumerable<T> where T: IComparable<T>
  public method Sort(): owned Array<T>
    var items := this.ToArray();
    // ...
    return items;
  end Sort;
end;
