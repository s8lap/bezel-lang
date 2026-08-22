extension IEnumerable<T>
  public method Map<U>(transform: (T) -> U): owned Array<U>
    var result := new Array<U>();
    for var item in this do
      result.Add(transform(item));
    end;
    return result;
  end Map;
end;
