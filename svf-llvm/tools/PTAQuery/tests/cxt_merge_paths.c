int pred(void);

int main(void)
{
    int a, b, c;
    int *pA, *pB, *pC;

    pA = &a;
    pB = &b;
    pC = &c;

    if (pred())
        pA = pred() ? pB : pC;

    int *pD = pA;
    return 0;
}
