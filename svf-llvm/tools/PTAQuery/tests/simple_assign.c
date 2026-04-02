int main()
{
    int *p, a;
    p = &a;
    int *q, b;
    q = &b;
    q = p;
    return 0;
}
