int p;
int *q,*r;
int **s;

void foo()
{
    *s = &p;
}

void bar()
{
    s = &r;
}

int main()
{
    s = &q;
    foo();
    return 0;
}