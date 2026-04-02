int main()
{
    int *p, a, b, c;
    p = &a;
    if (*p == 142)
        p = &b;
    else
        p = &c;
    int d = *p;
    return d;
}
