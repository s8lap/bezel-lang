_ := expression;            // value evaluated and dropped on the floor;
                            //   only legal for non-disposing types.

using var _ := expression;  // value bound to an unnamed local that is
                            //   disposed at scope end; works for any
                            //   class type.
