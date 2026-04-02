#include<stdlib.h>
int *x, *y, *w, **z, a, b, c;

int* (*fp)();

int* Q() {
    int *p = malloc (sizeof(int));
    x = p;
    return x;
}

int* R() {
    int *q = malloc (sizeof(int));
    x = q;
    return x;
}

void P(int*aa){
  w = aa;
}

int main(){
    z = &x;
    int *s;
    if (a==10)
      fp = Q;
    else
      fp = R;

    s = fp();
    y = s;

    P(y);
    return *y;
}