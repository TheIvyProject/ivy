# 💬 Comments

Ivy supports two types of comments, identical to C and C++: single-line comments and multi-line (block) comments.

---

### 1. Single-Line Comments (`//`)

Single-line comments start with `//` and extend to the end of the line.

```ivy
// This is a single-line comment
int32 count = 0; // Initialize counter
```

---

### 2. Multi-Line (Block) Comments (`/* ... */`)

Multi-line comments start with `/*` and terminate with `*/`. They can span across multiple lines or be placed inline within an expression.

```ivy
/* This is a multi-line comment
   spanning across several lines */
void processData() {
    /* inline comment */ int32 value = 42;
}
```
