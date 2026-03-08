struct FunctionPointer{
    int (*fp)(int);
};

int f(int k){
    return k;
}

int g(int k){
    return k+1;
}

int main(){
    struct FunctionPointer fp;
    fp.fp = &f;
    fp.fp(1);
    fp.fp = &g;
    fp.fp(1);
}