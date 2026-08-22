interface IList<T>: IReadOnlyList<T>, ICollection<T>
  property [i: int]: T;
end IList;
