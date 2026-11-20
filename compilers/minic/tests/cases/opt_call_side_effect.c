// TEST: an unused call result is dropped but the call itself is kept
// EXPECT-STDOUT: 42
// EXPECT-EXIT: 0
// EXPECT-IR-OPT: call noisy(42)
int noisy(int x) { print(x); return x; }
int main() {
    int unused = noisy(42);
    return 0;
}
