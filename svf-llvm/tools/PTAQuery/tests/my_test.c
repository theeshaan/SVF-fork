int main()
{
    int *arr[5];
    int *p, *q, *r, *s;
    int a,b,c,d;
    p = &a;
    q = &b;
    r = &c;
    s = &d;

    arr[0] = p;
    arr[1] = q;
    arr[2] = r;
    arr[3] = s;

    int *tgt = arr[1];
}