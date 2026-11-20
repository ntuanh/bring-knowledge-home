// Error example: several semantic errors, all reported in one run
void log(int x) { print(x); }

int add(int a, int b) { return a + b; }

int main() {
    int a = 1;
    int a = 2;              // redeclaration in the same scope
    int c = b + 1;          // b was never declared
    int d = log(a);         // log returns void
    add(1);                 // wrong number of arguments
    3 = a;                  // left side is not a variable
    return missing(c);      // undefined function
}
