#include <stdio.h>

int main()
{
    int* p;
    int a, b, c, d, e, f;
    scanf("%d %d %d %d %d %d", &a, &b, &c, &d, &e, &f);

    if (a)
        p = &a;
    else if (b)
        p = &b;
    else if (c)
        p = &c;
    else if (d)
        p = &d;
    else if (e)
        p = &e;
    else
        p = &f;

    return 0;
}
