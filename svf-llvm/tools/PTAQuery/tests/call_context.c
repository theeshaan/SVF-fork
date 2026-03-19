struct S
{
    int x;
    int y;
};

struct S* foo(struct S* s)
{
    return s;
}

int main()
{
    struct S a;
    struct S b;
    struct S* pa = foo(&a);
    struct S* pb = foo(&b);
    return 0;
}
