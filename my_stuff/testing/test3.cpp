// Pointer aliasing
void t3()
{
    int x;
    int *p = &x;
    int *q = &x;
    int **y = &p;
}