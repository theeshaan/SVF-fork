struct Box
{
    int* slots[2];
};

int main()
{
    int a, b;
    struct Box box;
    box.slots[0] = &a;
    box.slots[1] = &b;
    int* p = box.slots[0];
    return 0;
}
