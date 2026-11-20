// Optimizer showcase: compile with -O1 and open the Optimizer stage
int square(int x) { return x * x; }

int main() {
    int width = 4 * 2;            // constant folding: 8
    int height = width + 2;       // constant propagation, then folding: 10
    int unused = width * 100;     // dead store: never read
    int debug = 0;
    if (debug) {                  // branch folding + unreachable code
        print(999);
    }
    int n = square(height);       // not folded: calls are not evaluated
    int a = n * 1 + 0;            // algebraic simplification: a = n
    int b = (n + a) * (n + a);    // common subexpression: n + a computed once
    print(b);                     // 40000
    return height;                // 10
}
