// Pointer aliasing

extern void MUSTALIAS(void*,void*);

void func()
{
    int x, y;
    int* p = &x;
    int* q = &y;
    q = p;
    MUSTALIAS(p, q);
}