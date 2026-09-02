# 👋 Hello world

Let's start with the traditional first program, "Hello World," in Ivy:

```ivy
import ivy.io

void main() {
    io::print("Hello World!");
}
```

The import statement imports other modules, and we want print which is in ivy.io module.

### Main Function Return Type

The `main` function in Ivy supports returning either `void` or `int32`:

- `void main()`: Does not require a return statement.
- `int32 main()`: Requires an explicit `return` statement with an integer value (e.g., `return 0;`).

```ivy
import ivy.io

int32 main() {
    io::print("Hello World!");
    return 0;
}
```

