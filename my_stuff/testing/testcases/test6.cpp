// Context sensitivity
int* id(int* p)
{
    return p;
}

void t6()
{
    int x, y;
    int* p = id(&x);
    int* q = id(&y);
}
