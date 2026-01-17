#include <cstdlib>
#include <cstdio>

struct A;
struct B;

/* function pointer types */
typedef void (*fp_t)(A*);
typedef int  (*fp2_t)(int);

/* simple functions */
void f1(A* a) { printf("f1\n"); }
void f2(A* a) { printf("f2\n"); }
int  g1(int x) { return x + 1; }
int  g2(int x) { return x * 2; }

/* nested struct with function pointer */
struct A {
    int x;
    fp_t fp;
};

/* struct containing:
 * - pointer to struct
 * - array of pointers
 * - array of structs
 * - function pointer
 */
struct B {
    A* pa;
    A* arr[2];
    A  objs[2];
    fp2_t fp2;
};

/* global objects */
A gA;
B gB;
A* gPtr;

void foo(B* b, int idx) {
    b->pa = &gA;                 // struct field pointer
    b->arr[idx] = b->pa;         // array inside struct
    b->objs[0].fp = f1;          // function pointer inside struct array
    b->objs[1].fp = f2;

    A* p = &b->objs[idx];
    p->fp(p);                    // indirect call via struct field
}

void bar(void* vp) {
    /* type punning / cast */
    B* b = (B*)vp;
    b->fp2 = g1;
    int (*q)(int) = b->fp2;      // copy function pointer
    printf("%d\n", q(10));
}

int main() {
    B* b1 = new B;               // heap allocation
    B* b2 = (B*)malloc(sizeof(B));

    gPtr = &gA;

    b1->pa = gPtr;
    b1->arr[0] = &b1->objs[0];
    b1->arr[1] = &b1->objs[1];

    b1->objs[0].fp = f1;
    b1->objs[1].fp = f2;

    b1->fp2 = g2;

    foo(b1, 1);
    bar((void*)b1);

    /* aliasing through pointer copy */
    B* alias = b1;
    A* ap = alias->arr[1];
    ap->fp(ap);

    /* pointer reassignment */
    b2->pa = ap;
    b2->pa->fp = f1;
    b2->pa->fp(b2->pa);

    delete b1;
    free(b2);
    return 0;
}
