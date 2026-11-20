// TEST: a repeated subexpression is computed once (local CSE)
// EXPECT-STDOUT: 30|30
// EXPECT-EXIT: 60
// EXPECT-IR-OPT-NOT: t2 = a * b
int get(int x) { return x; }
int main() {
    int a = get(5);
    int b = get(6);
    int x = a * b;
    int y = a * b;
    print(x);
    print(y);
    return x + y;
}
