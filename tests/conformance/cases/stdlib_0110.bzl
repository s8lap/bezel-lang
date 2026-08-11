public async method FailFast(): OwningTask<MyClass>
  throw new InvalidOperationException("no MyClass available");
end FailFast;
