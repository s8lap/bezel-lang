public extension IEnumerable<T> where T: INumber<T>
  public method Sum(): owned T;        // T.Zero()/`+` over §2.19; `owned` erased at value T (spec §28.1)
end;
