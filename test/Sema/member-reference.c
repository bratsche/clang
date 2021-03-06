// RUN: clang-cc %s -verify -fsyntax-only

struct simple { int i; };

void f(void) {
   struct simple s[1];
   s->i = 1;
}

typedef int x;
struct S {
  int x;
  x z;
};

void g(void) {
  struct S s[1];
  s->x = 1;
  s->z = 2;
}
