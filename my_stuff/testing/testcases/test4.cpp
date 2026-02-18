// Pointer aliasing
void t4()
{
    int x, y;
    int* p = &x;
    int* q = &y;
    q = p;
}