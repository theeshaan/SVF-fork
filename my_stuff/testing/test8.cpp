// Function pointers and conditional assignment
void f(int* p) {}
void g(int* p) {}

void t8_1()
{
    void (*fp)(int*);
    fp = f;
}


void t8_2(int c)
{
    void (*fp)(int*);
    int x;
    if (c)
        fp = f;
    else
        fp = g;
    fp(&x);
}

