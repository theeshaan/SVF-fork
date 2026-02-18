// A program to test the precision loss due to callstrings length limit in SVF
// Callstrings limit=3 (default) => pa->{a,b}, pb->{a,b}
// Callstrings limit=5 => pa->{a}, pb->{b} (set by dvf option -max-cxt=5)

struct Obj {int value;};

Obj* id5(Obj* in) {return in;}
Obj* id4(Obj* in) {return id5(in);}
Obj* id3(Obj* in) {return id4(in);}
Obj* id2(Obj* in) {return id3(in);}
Obj* id1(Obj* in) {return id2(in);}

int main()
{
    Obj a;
    Obj b;

    Obj* pa = id1(&a);  // Call context 1
    Obj* pb = id1(&b);  // Call context 2
}