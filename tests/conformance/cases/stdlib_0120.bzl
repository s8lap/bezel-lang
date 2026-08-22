public static async method WhenAll<T>(
  owned tasks: OwningArray<OwningTask<T>>
): OwningTask<OwningArray<T>> where T: class
  using var taskArray := move tasks;                  // owned param adopted (transfer marker); disposes at end
  var results := new OwningArray<T>(capacity := taskArray.Count);
  for var task in taskArray.Drain() do                // drain yields owned OwningTask<T>
    results.Add(await task);                          // await produces owned T (Pattern B);
                                                      //   Add(owned item) is an owned-consuming
                                                      //   context, satisfying §30.5
  end;
  return results;                                     // ownership transfers to caller
end WhenAll;
