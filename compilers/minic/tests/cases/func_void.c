// TEST: void functions, early return, and falling off the end of a function
// EXPECT-STDOUT: 3|2|1|0
// EXPECT-EXIT: 0
void countdown(int n) {
    while (1) {
        print(n);
        if (n == 0) return;
        n = n - 1;
    }
}
void nothing() { }
int main() {
    countdown(3);
    nothing();
}
