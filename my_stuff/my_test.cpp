class A
{
public:
    int *p;
};

int main()
{
    A a, *b;
    int x;
    a.p = &x;
    b = &a;
}