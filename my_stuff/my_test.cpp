#include <iostream>
using namespace std;

// writing a code in which structures have function pointers in them
struct A {
    int x;
    void (*fp)(int);
};

void f1(int y) {
    cout << "f1: " << y << endl;
}

void f2(int z) {
    cout << "f2: " << z << endl;
}

int main() {
    A a;
    a.x = 1;
    a.fp = f1;
    a.fp(a.x);
    a.fp = f2;
    a.fp(10);
    return 0;
}