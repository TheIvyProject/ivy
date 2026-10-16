// E5 test: legacy .cpp extension is rejected by the CLI.
// This test verifies that passing a .cpp/.cc/.cxx/.c file to ivy
// reports an error directing the user to use the .ivy extension.
//
// EXPECT: immediate failure (exit 2) with error message containing
// "no longer accepted" and ".ivy".
//
// NOTE: this file is intentionally NOT valid Ivy code — it must never
// reach the lexer/parser. The CLI rejects it by extension first.

#pragma ivy cnumber

int main() {
    int x = 42;
    return x;
}
