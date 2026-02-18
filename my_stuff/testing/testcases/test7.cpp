// Context-sensitivity
int* foo(int* p)
{
    return p;
}

void t7()
{
    int x;
    foo(&x);
    foo(&x);
}
