#include <stdlib.h>

struct Node {
    struct Node* next;
};

struct Node* getNext(struct Node* n)
{
    return n->next;
}

int main(void)
{
    struct Node* head = (struct Node*)malloc(sizeof(struct Node));
    head->next = (struct Node*)malloc(sizeof(struct Node));
    struct Node* p = getNext(head);
    return p == NULL;
}
