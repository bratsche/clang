// RUN: clang -fsyntax-only -verify -std=c++0x %s

int && r1(int &&a);

typedef int && R;
void r2(const R a);