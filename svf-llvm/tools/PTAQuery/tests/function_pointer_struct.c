struct FunctionPointers
{
    void (*func1)(int);
    int (*func2)(double);
};

void exampleFunction1(int x)
{
}

int exampleFunction2(double y)
{
    return y / 1;
}

int main()
{
    struct FunctionPointers fp;
    fp.func1 = exampleFunction1;
    fp.func2 = exampleFunction2;

    fp.func1(42);
    int result = fp.func2(3.14);
    return result;
}
