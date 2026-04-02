#include <stdio.h>
int main()
{
    int a = 10;
    int b = 20;

    int *pA = &a;
    int *pB = &b;
    pA = pB;
}