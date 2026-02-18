// Pointer aliasing

extern void MUSTALIAS(void*,void*);

void t4()
{
    int x, y;
    int* p = &x;
    int* q = &y;
    q = p;
    MUSTALIAS(p, q);
}