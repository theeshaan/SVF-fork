#include <stdio.h>
#include <stdlib.h>

struct Node {
    int data;
    struct Node* left;
    struct Node* right;
};

int main(void)
{
    struct Node* a = (struct Node*)malloc(sizeof(struct Node));
    struct Node* b;
    struct Node* c = (struct Node*)malloc(sizeof(struct Node));

    a->left = (struct Node*)malloc(sizeof(struct Node));
    a->right = (struct Node*)malloc(sizeof(struct Node));

    b->left = NULL;
    b->right = (struct Node*)malloc(sizeof(struct Node));

    b->right->left = NULL;
    b->right->right = (struct Node*)malloc(sizeof(struct Node));

    b = a->left;
    c = b->right->right;

    printf("Data : %d", c->data);
    return 0;
}
