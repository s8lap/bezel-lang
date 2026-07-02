public async method LoadResourceAsync(path: string): OwningTask<Resource>
  using var tcs := new OwningTaskCompletionSource<Resource>();

  StartLoadInBackground(path, (loaded: owned Resource) => do
    tcs.TrySetResult(loaded);                      // ownership moves into tcs's task
  end);

  return await tcs.Task;                            // moves ownership out of tcs's task
                                                    //   and into the outer Pattern B task's
                                                    //   result slot. tcs is then disposed,
                                                    //   but its task already has no result.
end LoadResourceAsync;
