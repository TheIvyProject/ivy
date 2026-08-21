// Test: scoped enum type in function signature — should be mangled
// as a nested type name, not collapsed to the underlying int.
#pragma ivy cnumber

enum class Color { Red, Green, Blue };

// `take_color` takes a scoped enum — its mangled symbol must encode
// the enum type, not the underlying integer type.
int take_color(Color c) {
    return 1;
}

enum Shade { Light, Dark };  // unscoped enum

// `take_shade` takes an unscoped enum — its mangled symbol encodes
// the underlying integer type (per both Itanium and MSVC ABIs).
int take_shade(Shade s) {
    return 2;
}

int main() {
    return take_color(Color::Green) + take_shade(Light);
}
