// TEST: syntax error - an extra closing brace at top level
// EXPECT-ERROR: expected 'int' or 'void' at the start of a function, found '}'
int main() {
    return 0;
}
}
