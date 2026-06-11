extension IEnumerable<T>
  public method Map<U>(transform: (T) -> U): IEnumerable<U>
    var result := new Array<U>();
    for var item in this do
      result.Add(transform(item));
    end;
    return result;
  end Map;
end;
