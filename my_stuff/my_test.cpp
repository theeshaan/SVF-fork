#include <iostream>

struct Obj {
    int value;
};

Obj* id(Obj* p) {
    // Identity function
    return p;
}

int main() {
    Obj a;
    Obj b;

    Obj* pa = id(&a);  // Call context 1
    Obj* pb = id(&b);  // Call context 2

    return 0;
}
