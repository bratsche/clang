// Used with the struct.c test

struct Point {
  float x, y, z;
};

struct Point2 {
  float xValue, yValue, zValue;
};

struct Fun;

struct Fun *fun;

struct Fun {
  int is_ptr;

  union {
    void *ptr;
    int  *integer;
  };
};

struct Fun2;
struct Fun2 *fun2;